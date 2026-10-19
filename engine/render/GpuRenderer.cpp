#include "render/GpuRenderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <future>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

#include "core/FileSystem.h"
#include "core/Log.h"
#include "assets/Assets.h"
#include "render/ColorSpace.h"
#include "render/GpuDevice.h"
#include "render/GraphSource.h"
#include "render/RenderScene.h"
#include "render/PostProcess.h"
#include "render/PlanarReflection.h"
#include "render/Decals.h"
#include "render/UI.h"
#include "render/shaders/Shaders.glsl.h"
#include "scene/Components.h"

namespace oe {

namespace {

constexpr sg_pixel_format kColorFormat = SG_PIXELFORMAT_RGBA8;
constexpr sg_pixel_format kHdrFormat = SG_PIXELFORMAT_RGBA16F;
constexpr sg_pixel_format kDepthFormat = SG_PIXELFORMAT_DEPTH;
constexpr sg_pixel_format kDofDepthFormat = SG_PIXELFORMAT_R32F;  // depth buffer values read by the depth-of-field passes
constexpr int kMaxDirLights = 4;
constexpr int kMaxPointLights = 16;
const Color kOutlineColor(1.0f, 0.62f, 0.1f);

// composite_fs tone_mapping.y: 0 none, 1 Reinhard, 2 ACES.
float ToneMappingMode(const PostProcess& post) {
    return post.toneMapping == "aces" ? 2.0f : post.toneMapping == "reinhard" ? 1.0f : 0.0f;
}

// ----- Shader bytecode (devices that compile source themselves, e.g. D3D11) -----

// One program's two stages: source in, bytecode out.
struct ProgramCode {
    struct Stage {
        std::string source, target, entry;
        std::vector<uint8_t> bytecode;
    };
    Stage vs, fs;
    bool ok = false;  // both stages have bytecode
};

uint64_t Fnv1a(const std::string& text, uint64_t hash = 1469598103934665603ull) {
    for (unsigned char c : text) hash = (hash ^ c) * 1099511628211ull;
    return hash;
}

ProgramCode::Stage StageOf(const sg_shader_function& function, const char* defaultTarget) {
    ProgramCode::Stage stage;
    stage.source = function.source ? function.source : "";
    stage.target = function.d3d11_target ? function.d3d11_target : defaultTarget;
    stage.entry = function.entry ? function.entry : "main";
    return stage;
}

// Bytecode of one stage from `cacheDir` (file name = hash of compiler, target, entry and source),
// else compiled and stored there. Any thread.
bool CompileStage(const GpuDevice& device, const std::string& compiler, const std::string& cacheDir, ProgramCode::Stage& stage) {
    char name[32];
    const uint64_t key = Fnv1a(stage.source, Fnv1a(compiler + "\n" + stage.target + "\n" + stage.entry));
    std::snprintf(name, sizeof(name), "%016llx.bin", static_cast<unsigned long long>(key));
    const std::string path = cacheDir.empty() ? std::string() : JoinPath(cacheDir, name);
    std::vector<unsigned char> cached;
    if (!path.empty() && ReadBinaryFile(path, cached) && !cached.empty()) {
        stage.bytecode.assign(cached.begin(), cached.end());
        return true;
    }
    std::string error;
    if (!device.CompileShader(stage.source, stage.target.c_str(), stage.entry.c_str(), &stage.bytecode, &error)) {
        OE_LOG_WARN("gpu", "shader compile failed: %s", error.c_str());
        return false;
    }
    if (!path.empty()) {
        // Programs that share a stage compile it on several threads at once; WriteTextFileAtomic
        // writes through one fixed temporary name, so concurrent writers of a key could lose the file.
        static std::mutex writing;
        std::lock_guard<std::mutex> lock(writing);
        WriteTextFileAtomic(path, std::string(stage.bytecode.begin(), stage.bytecode.end()), nullptr);
    }
    return true;
}

bool CompileProgram(const GpuDevice& device, const std::string& compiler, const std::string& cacheDir, ProgramCode& code) {
    code.ok = !code.vs.source.empty() && !code.fs.source.empty() && CompileStage(device, compiler, cacheDir, code.vs) &&
              CompileStage(device, compiler, cacheDir, code.fs);
    return code.ok;
}

// `desc` with the compiled stages in place of its source.
sg_shader MakeCompiledShader(sg_shader_desc desc, const ProgramCode& code) {
    desc.vertex_func.source = nullptr;
    desc.vertex_func.bytecode = {code.vs.bytecode.data(), code.vs.bytecode.size()};
    desc.fragment_func.source = nullptr;
    desc.fragment_func.bytecode = {code.fs.bytecode.data(), code.fs.bytecode.size()};
    return sg_make_shader(&desc);
}

// Keeps a shader cache folder bounded: the oldest files go first.
void TrimShaderCache(const std::string& dir, size_t keep) {
    if (dir.empty() || !IsDirectory(dir)) return;
    std::vector<std::string> files = ListFiles(dir, ".bin", false);
    if (files.size() <= keep) return;
    std::vector<std::pair<int64_t, std::string>> dated;
    for (const std::string& file : files) dated.emplace_back(FileModifiedTime(file), file);
    std::sort(dated.begin(), dated.end());
    for (size_t i = 0; i + keep < dated.size(); ++i) RemoveAll(dated[i].second);
}

struct MeshVertex {
    float pos[3];
    float nrm[3];
    float uv[2];
    float tan[4];
    float joints[4];
    float weights[4];
    float color[4];
    float particle[4];
    float custom[4];
};

struct ColorVertex {
    float pos[3];
    float color[4];
};

struct UIVertex {
    float pos[2];
    float uv[2];
    float color[4];
    float border[4];
    float rect[4];
    float params[4];  // radius, border width, textured, shaped
};

void Put(float* dst, const Mat4& m) { std::memcpy(dst, m.m, sizeof(m.m)); }
void Put(float* dst, float x, float y, float z, float w) {
    dst[0] = x;
    dst[1] = y;
    dst[2] = z;
    dst[3] = w;
}

// One instruction of the mesh shaders' interpreter as GraphCode in Shaders.glsl unpacks it.
static_assert(static_cast<int>(ShaderOp::ObjectPosition) < 128, "GraphCode packs the opcode in 7 bits");
static_assert(ShaderGraph::kMaxInterpretedSurfaceNodes <= 48, "GraphCode packs register indices base 48");
float PackGraphCode(const ShaderInstruction& instruction) {
    return static_cast<float>(static_cast<int>(instruction.op) +
                              128 * (instruction.args[0] + 48 * (instruction.args[1] + 48 * instruction.args[2])));
}

// `compiled`: the pipeline runs the graph's compiled program, which needs no interpreter instructions and has the
// light stage (`lightStage`). Without it (still compiling in the background, failed) the interpreter runs graphs
// of up to kMaxInterpretedSurfaceNodes, a larger one draws as its plain material, and a graph with a light output
// is shaded by the standard lighting. `viewer`: the camera (ShaderViewer).
void PutGraph(oe_mesh_material_t& uniforms, const Material& material, float time, const Vec4& viewer, bool compiled, bool lightStage) {
    if (!material.shader) return;
    const ShaderGraph& graph = *material.shader;
    const bool interpreted = !compiled && graph.instructions.size() <= static_cast<size_t>(ShaderGraph::kMaxInterpretedSurfaceNodes);
    if (!compiled && !interpreted) return;
    Put(uniforms.graph_control, static_cast<float>(graph.instructions.size()), static_cast<float>(graph.color),
        static_cast<float>(graph.emissive), time);
    Put(uniforms.graph_outputs, static_cast<float>(graph.normal), lightStage && compiled ? static_cast<float>(graph.light) : -1.0f, 0, 0);
    Put(uniforms.graph_viewer, viewer.x, viewer.y, viewer.z, viewer.w);
    for (size_t node = 0; interpreted && node < graph.instructions.size(); ++node) {
        const ShaderInstruction& instruction = graph.instructions[node];
        uniforms.graph_code[node / 4][node % 4] = PackGraphCode(instruction);
        Put(uniforms.graph_value[node], instruction.value.x, instruction.value.y, instruction.value.z, instruction.value.w);
    }
    for (size_t slot = 0; slot < material.shaderUniforms.size(); ++slot) {
        const Vec4& value = material.shaderUniforms[slot];
        Put(uniforms.graph_uniform[slot], value.x, value.y, value.z, value.w);
    }
}

// True when the material's graph moves vertices: its depth, shadow and mask draws need mesh_vs too.
bool Displaced(const Material& material) { return material.shader && material.shader->offset >= 0; }

// Last index of meshPips/hdrMeshPips: which faces the scene pass draws.
int FaceMode(const Material& material) { return material.cullFront ? 2 : material.doubleSided ? 1 : 0; }

// Vertex stage of a graph with an `offset` output: the instructions up to that output (`compiled` as for PutGraph).
void PutVertexGraph(oe_mesh_vs_graph_t& uniforms, const Material& material, const RenderItem& item, float time, const Vec4& viewer,
                    bool compiled) {
    if (!Displaced(material)) return;
    const ShaderGraph& graph = *material.shader;
    const size_t count = static_cast<size_t>(graph.offset) + 1;
    const bool interpreted = !compiled && count <= static_cast<size_t>(ShaderGraph::kMaxInterpretedSurfaceNodes);
    if (!compiled && !interpreted) return;
    Put(uniforms.vgraph_control, static_cast<float>(count), static_cast<float>(graph.offset), 0, time);
    Put(uniforms.vgraph_base_color, material.baseColor.r, material.baseColor.g, material.baseColor.b, material.opacity);
    Put(uniforms.vgraph_uv_rect, item.uvOffset[0], item.uvOffset[1], item.uvScale[0], item.uvScale[1]);
    Put(uniforms.vgraph_uv_tiling, material.tiling[0], material.tiling[1], material.offset[0], material.offset[1]);
    Put(uniforms.vgraph_viewer, viewer.x, viewer.y, viewer.z, viewer.w);
    for (size_t node = 0; interpreted && node < count; ++node) {
        const ShaderInstruction& instruction = graph.instructions[node];
        uniforms.vgraph_code[node / 4][node % 4] = PackGraphCode(instruction);
        Put(uniforms.vgraph_value[node], instruction.value.x, instruction.value.y, instruction.value.z, instruction.value.w);
    }
    for (size_t slot = 0; slot < material.shaderUniforms.size(); ++slot) {
        const Vec4& value = material.shaderUniforms[slot];
        Put(uniforms.vgraph_uniform[slot], value.x, value.y, value.z, value.w);
    }
}

// Pixel format of Texture::texels (bytes R, G, B, A) is RGBA8. Mip levels
// are box filtered on the CPU so distant textures do not shimmer; `srgb` (an SRGB8A8 texture) averages the colors
// as linear light and encodes the result again, as GPUs filter such textures (alpha is linear either way).
std::vector<std::vector<uint32_t>> BuildMips(const Texture& tex, bool srgb) {
    static const std::array<float, 256> kDecoded = [] {
        std::array<float, 256> table{};
        for (int i = 0; i < 256; ++i) table[static_cast<size_t>(i)] = SrgbToLinear(static_cast<float>(i) / 255.0f);
        return table;
    }();
    std::vector<std::vector<uint32_t>> mips;
    mips.push_back(tex.texels);
    int w = tex.width, h = tex.height;
    while ((w > 1 || h > 1) && static_cast<int>(mips.size()) < SG_MAX_MIPMAPS) {
        int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        const std::vector<uint32_t>& src = mips.back();
        std::vector<uint32_t> dst(static_cast<size_t>(nw) * static_cast<size_t>(nh));
        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                uint32_t sum[4] = {0, 0, 0, 0};
                float light[3] = {0, 0, 0};
                for (int k = 0; k < 4; ++k) {
                    int sx = std::min(w - 1, x * 2 + (k & 1)), sy = std::min(h - 1, y * 2 + (k >> 1));
                    uint32_t c = src[static_cast<size_t>(sy) * static_cast<size_t>(w) + static_cast<size_t>(sx)];
                    for (int ch = 0; ch < 4; ++ch) sum[ch] += (c >> (8 * ch)) & 0xFF;
                    for (int ch = 0; srgb && ch < 3; ++ch) light[ch] += kDecoded[(c >> (8 * ch)) & 0xFF];
                }
                uint32_t out = 0;
                for (int ch = 0; ch < 4; ++ch) {
                    const uint32_t value = srgb && ch < 3 ? static_cast<uint32_t>(LinearToSrgb(light[ch] / 4) * 255.0f + 0.5f)
                                                          : (sum[ch] + 2) / 4;
                    out |= value << (8 * ch);
                }
                dst[static_cast<size_t>(y) * static_cast<size_t>(nw) + static_cast<size_t>(x)] = out;
            }
        }
        mips.push_back(std::move(dst));
        w = nw;
        h = nh;
    }
    return mips;
}

// Resources whose lifetime follows a CPU object owned by the asset manager.
// Built-in meshes have no owner (use_count 0) and live as long as the renderer.
template <class T>
bool OwnerAlive(bool permanent, const std::weak_ptr<const T>& owner) {
    return permanent || !owner.expired();
}

struct GpuMesh {
    sg_buffer vbuf{};
    sg_buffer ibuf{};
    std::weak_ptr<const Mesh> owner;
    bool permanent = false;
    void Destroy() {
        sg_destroy_buffer(vbuf);
        sg_destroy_buffer(ibuf);
    }
};

struct GpuTexture {
    sg_image image{};
    sg_view view{};
    uint32_t version = 0;
    std::weak_ptr<const Texture> owner;
    bool permanent = false;
    void Destroy() {
        sg_destroy_view(view);
        sg_destroy_image(image);
    }
};

// GPU copy of an environment image: every blur level as a mip level of one texture.
struct GpuEnvironment {
    sg_image image{};
    sg_view view{};
    std::weak_ptr<const EnvironmentMap> owner;
    bool permanent = false;
    void Destroy() {
        sg_destroy_view(view);
        sg_destroy_image(image);
    }
};

// A non-negative float as a 16-bit half float (rounded to nearest; above the range gives the largest half).
uint16_t HalfFloat(float value) {
    value = std::isfinite(value) ? std::min(std::max(value, 0.0f), 65504.0f) : 0.0f;
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const int exponent = static_cast<int>((bits >> 23) & 0xFF) - 127 + 15;
    uint32_t mantissa = bits & 0x7FFFFF;
    if (exponent <= 0) {  // too small for a normal half: a subnormal one, or zero
        if (exponent < -10) return 0;
        mantissa |= 0x800000;
        const uint32_t shift = static_cast<uint32_t>(14 - exponent);
        return static_cast<uint16_t>((mantissa >> shift) + ((mantissa >> (shift - 1)) & 1));
    }
    return static_cast<uint16_t>(((static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13)) + ((mantissa >> 12) & 1));
}

// Where a draw's geometry lives: its mesh's buffers, or a range of this frame's batch streams.
struct MeshBinding {
    sg_buffer vbuf{}, ibuf{};
    int vbufOffset = 0, ibufOffset = 0;  // bytes
    int firstIndex = 0, indexCount = 0;
};

MeshBinding Binding(const GpuMesh& mesh, const Submesh& sub) {
    MeshBinding b;
    b.vbuf = mesh.vbuf;
    b.ibuf = mesh.ibuf;
    b.firstIndex = static_cast<int>(sub.firstIndex);
    b.indexCount = static_cast<int>(sub.indexCount);
    return b;
}

void Bind(sg_bindings& b, const MeshBinding& mesh) {
    b.vertex_buffers[0] = mesh.vbuf;
    b.vertex_buffer_offsets[0] = mesh.vbufOffset;
    b.index_buffer = mesh.ibuf;
    b.index_buffer_offset = mesh.ibufOffset;
}

void Draw(const MeshBinding& mesh) { sg_draw(mesh.firstIndex, mesh.indexCount, 1); }

// Dynamic batching (as Unity does for small meshes): a run of consecutive draws of small static
// meshes (sprites, particle quads) whose materials differ at most in color and opacity is copied
// into one world-space vertex stream per frame and drawn with one call. The per-item values the
// shaders take as uniforms move into the vertices: position, normal and tangent in world space,
// uv with the sprite frame applied, color * base color / opacity in color0, particle inputs and
// custom channels added to particle0/custom0. The batch draws with identity transforms, a white
// base color and the identity uv rect, which gives each vertex what its own draw would compute.
constexpr size_t kBatchVertices = 64, kBatchIndices = 192, kBatchDraws = 1024;

struct DrawPiece {
    size_t draw = 0;  // first draw of the piece (draw list index)
    int batch = -1;   // >= 0: the piece is batches[batch]
};
struct DrawBatch {
    DrawCall call;     // the first draw's, with a white base color and opacity 1
    RenderItem item;   // identity transforms and uv rect, no joints
    MeshBinding mesh;  // range of the frame's batch streams
};
struct DrawBatches {
    std::vector<DrawBatch> batches;
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
};

// Sprite/Tilemap 2D extras of mesh_vs (light map position, mask, ambient, normal map flips).
void PutSprite2D(oe_mesh_vs_params_t& vu, const Material& m, bool lightMap, bool maskImage) {
    if (m.light2DOn) {
        if (lightMap && m.light2D)
            Put(vu.sprite_light, m.light2D->x0, m.light2D->y0, 1.0f / m.light2D->cellW, 1.0f / m.light2D->cellH);
        Put(vu.sprite_ambient, m.ambient2D.r, m.ambient2D.g, m.ambient2D.b, 0);
        Put(vu.sprite_flip, m.flip2D[0], m.flip2D[1], 0, 0);
    }
    if (m.mask2DOn) {
        Put(vu.sprite_mask_x, m.mask2DX.x, m.mask2DX.y, m.mask2DX.z, m.mask2DX.w);
        Put(vu.sprite_mask_y, m.mask2DY.x, m.mask2DY.y, m.mask2DY.z, m.mask2DY.w);
        Put(vu.sprite_mask_uv, m.mask2DUv.x, m.mask2DUv.y, m.mask2DUv.z, m.mask2DUv.w);
    }
    const float light = m.light2DOn ? (lightMap && m.light2D ? 2.0f : 1.0f) : 0.0f;
    const float mask = !m.mask2DOn ? 0.0f : !m.mask2DFound ? 1.0f : maskImage ? 3.0f : 2.0f;
    Put(vu.sprite_state, light, mask, Clamp(m.mask2DCutoff, 0.0f, 1.0f), 0);
}

bool SameBatchMaterial(const Material& a, const Material& b) {
    return a.baseTexture == b.baseTexture && a.metallic == b.metallic && a.roughness == b.roughness &&
           a.metallicRoughnessTexture == b.metallicRoughnessTexture && a.normalTexture == b.normalTexture &&
           a.normalScale == b.normalScale && a.occlusionTexture == b.occlusionTexture && a.occlusionStrength == b.occlusionStrength &&
           a.emissive == b.emissive && a.emissiveIntensity == b.emissiveIntensity && a.emissiveTexture == b.emissiveTexture &&
           a.alphaMode == b.alphaMode && a.alphaCutoff == b.alphaCutoff && a.doubleSided == b.doubleSided && a.additive == b.additive &&
           a.cullFront == b.cullFront && a.castShadows == b.castShadows && !a.smoothNormals && !b.smoothNormals &&
           a.unlit == b.unlit && a.pixelArt == b.pixelArt && a.fog == b.fog && a.tiling[0] == b.tiling[0] && a.tiling[1] == b.tiling[1] &&
           a.offset[0] == b.offset[0] && a.offset[1] == b.offset[1] && a.shader == b.shader && a.light2DOn == b.light2DOn &&
           a.light2D == b.light2D && a.ambient2D == b.ambient2D && a.flip2D[0] == b.flip2D[0] && a.flip2D[1] == b.flip2D[1] &&
           !a.mask2DOn && !b.mask2DOn && !(a.shader && a.shader->objectSpace) &&
           std::memcmp(a.shaderUniforms.data(), b.shaderUniforms.data(), sizeof(a.shaderUniforms)) == 0;
}

bool Batchable(const RenderItem& item, const Submesh& sub) {
    return item.joints.empty() && !item.flat && item.mesh->positions.size() <= kBatchVertices && sub.indexCount <= kBatchIndices;
}

MeshVertex BatchVertex(const Mesh& m, uint32_t i, const RenderItem& item, const Material& material, bool linear) {
    MeshVertex v{};
    const Vec3 p = item.world.TransformPoint(m.positions[i]);
    const Vec3 n = item.normalMatrix.TransformDir(i < m.normals.size() ? m.normals[i] : Vec3(0, 1, 0));
    const Vec4 t = i < m.tangents.size() ? m.tangents[i] : Vec4(1, 0, 0, 1);
    const Vec3 tw = item.world.TransformDir(t.xyz());
    const Vec4 c = m.VertexColor(i, linear);
    const Vec4 data = (i < m.particleData.size() ? m.particleData[i] : Vec4()) + item.particleInput;
    const Vec4 custom = (i < m.particleCustom.size() ? m.particleCustom[i] : Vec4()) + item.particleCustom;
    const size_t uv = static_cast<size_t>(i) * 2;
    v.pos[0] = p.x;
    v.pos[1] = p.y;
    v.pos[2] = p.z;
    v.nrm[0] = n.x;
    v.nrm[1] = n.y;
    v.nrm[2] = n.z;
    v.uv[0] = (uv + 1 < m.uvs.size() ? m.uvs[uv] : 0.0f) * item.uvScale[0] + item.uvOffset[0];
    v.uv[1] = (uv + 1 < m.uvs.size() ? m.uvs[uv + 1] : 0.0f) * item.uvScale[1] + item.uvOffset[1];
    Put(v.tan, tw.x, tw.y, tw.z, t.w);
    Put(v.color, c.x * material.baseColor.r, c.y * material.baseColor.g, c.z * material.baseColor.b, c.w * material.opacity);
    Put(v.particle, data.x, data.y, data.z, data.w);
    Put(v.custom, custom.x, custom.y, custom.z, custom.w);
    return v;
}

// Splits `order` (draw list indices, in drawing order) into single draws and batches; batch geometry
// is appended to `out`. Only consecutive draws are joined, so the drawing order stays the same.
std::vector<DrawPiece> PlanBatches(const std::vector<RenderItem>& items, const std::vector<DrawCall>& draws,
                                   const std::vector<size_t>& order, DrawBatches& out, EntityId separate = kNullEntity, bool linear = false) {
    std::vector<DrawPiece> pieces;
    for (size_t i = 0; i < order.size();) {
        const DrawCall& first = draws[order[i]];
        const RenderItem& head = items[first.item];
        size_t j = i + 1;
        if (head.id != separate && Batchable(head, head.mesh->submeshes[first.submesh])) {
            while (j < order.size() && j - i < kBatchDraws) {
                const DrawCall& next = draws[order[j]];
                const RenderItem& it = items[next.item];
                if (it.id == separate || !Batchable(it, it.mesh->submeshes[next.submesh]) || next.blend != first.blend ||
                    !SameBatchMaterial(next.material, first.material)) break;
                ++j;
            }
        }
        if (j - i < 2) {
            pieces.push_back({order[i], -1});
            ++i;
            continue;
        }
        DrawBatch batch;
        batch.call = first;
        batch.call.material.baseColor = Color(1, 1, 1);
        batch.call.material.opacity = 1.0f;
        batch.item.mesh = head.mesh;  // a valid mesh; the batch's geometry is in the streams
        const size_t firstVertex = out.vertices.size(), firstIndex = out.indices.size();
        batch.mesh.vbufOffset = static_cast<int>(firstVertex * sizeof(MeshVertex));
        batch.mesh.ibufOffset = static_cast<int>(firstIndex * sizeof(uint32_t));
        for (size_t k = i; k < j; ++k) {
            const DrawCall& dc = draws[order[k]];
            const RenderItem& it = items[dc.item];
            const Mesh& m = *it.mesh;
            const Submesh& sub = m.submeshes[dc.submesh];
            const uint32_t base = static_cast<uint32_t>(out.vertices.size() - firstVertex);
            for (uint32_t v = 0; v < m.positions.size(); ++v) out.vertices.push_back(BatchVertex(m, v, it, dc.material, linear));
            for (uint32_t x = 0; x < sub.indexCount; ++x) out.indices.push_back(base + m.indices[sub.firstIndex + x]);
        }
        batch.mesh.indexCount = static_cast<int>(out.indices.size() - firstIndex);
        pieces.push_back({order[i], static_cast<int>(out.batches.size())});
        out.batches.push_back(std::move(batch));
        i = j;
    }
    return pieces;
}

// A streamed vertex (or index) buffer rewritten every frame (lines, UI, batches).
struct StreamBuffer {
    sg_buffer buf{};
    size_t capacity = 0;
    bool index = false;
    // Writes `bytes` and returns false when there is nothing to draw.
    bool Write(const void* data, size_t bytes) {
        if (bytes == 0) return false;
        if (bytes > capacity) {
            if (capacity) sg_destroy_buffer(buf);
            capacity = std::max<size_t>(bytes, std::max<size_t>(capacity * 2, 64 * 1024));
            sg_buffer_desc d{};
            d.size = capacity;
            d.usage.vertex_buffer = !index;
            d.usage.index_buffer = index;
            d.usage.write_transient = true;
            d.label = "oe-stream";
            buf = sg_make_buffer(&d);
        }
        sg_write_buffer_desc w{};
        w.src.data = {data, bytes};
        w.dst.buffer = buf;
        w.size = bytes;
        sg_write_buffer_transient(&w);
        return true;
    }
    void Destroy() {
        if (capacity) sg_destroy_buffer(buf);
        capacity = 0;
    }
};

sg_image MakeAttachmentImage(int w, int h, sg_pixel_format format, int samples, bool depth, bool resolve, const char* label) {
    sg_image_desc d{};
    d.width = w;
    d.height = h;
    d.pixel_format = format;
    d.sample_count = samples;
    if (depth) d.usage.depth_stencil_attachment = true;
    else if (resolve) d.usage.resolve_attachment = true;
    else d.usage.color_attachment = true;
    d.label = label;
    return sg_make_image(&d);
}

sg_view ColorView(sg_image img) {
    sg_view_desc d{};
    d.color_attachment.image = img;
    return sg_make_view(&d);
}
sg_view ResolveView(sg_image img) {
    sg_view_desc d{};
    d.resolve_attachment.image = img;
    return sg_make_view(&d);
}
sg_view DepthView(sg_image img) {
    sg_view_desc d{};
    d.depth_stencil_attachment.image = img;
    return sg_make_view(&d);
}
sg_view TextureView(sg_image img) {
    sg_view_desc d{};
    d.texture.image = img;
    return sg_make_view(&d);
}

// Offscreen images for one output size.
struct Targets {
    int width = 0, height = 0;
    int msaa = 1;
    bool withOutput = false;
    bool hdr = false;
    bool bloom = false;
    bool beforeGraph = false;
    sg_image postColor{};
    sg_view postAtt{}, postTex{};
    bool fxaa = false;
    bool dof = false;
    bool ssr = false;
    sg_image ssrSurface[3]{}, ssrMsaa[3]{}, ssrColor{};
    sg_view ssrAtt[3]{}, ssrMsaaAtt[3]{}, ssrTex[3]{}, ssrColorAtt{}, ssrColorTex{};
    bool ssao = false;
    sg_image ssaoDepth{};
    sg_view ssaoDepthAtt{}, ssaoDepthTex{};
    // Depth of field: scene depth as a color, then the two blur passes in the scene format. With `dofMrt`
    // the scene pass writes view depth into a second (multisampled, resolved) RGBA16F target; otherwise
    // a separate single-sample pass draws the scene again for its depth buffer values.
    bool dofMrt = false;
    sg_image dofDepth{}, dofDepthMsaa{}, dofHorizontal{}, dofVertical{};
    sg_view dofDepthAtt{}, dofDepthMsaaAtt{}, dofDepthResolveAtt{}, dofDepthTex{}, dofHorizontalAtt{}, dofHorizontalTex{}, dofVerticalAtt{}, dofVerticalTex{};
    sg_image sceneMsaa{}, sceneDepth{}, sceneColor{}, maskColor{}, maskDepth{}, output{};
    sg_image bloomHorizontal{}, bloomVertical{};
    sg_image fxaaColor{};
    sg_view fxaaAtt{}, fxaaTex{};
    sg_view sceneMsaaAtt{}, sceneDepthAtt{}, sceneColorAtt{}, sceneTex{}, maskAtt{}, maskDepthAtt{}, maskTex{}, outputAtt{}, outputTex{};
    sg_view bloomHorizontalAtt{}, bloomHorizontalTex{}, bloomVerticalAtt{}, bloomVerticalTex{};

    bool Matches(int w, int h, bool output_, bool hdr_, bool bloom_, bool fxaa_, bool dof_, bool dofMrt_, bool ssao_, bool ssr_, bool beforeGraph_) const {
        return width == w && height == h && withOutput == output_ && hdr == hdr_ && bloom == bloom_ && fxaa == fxaa_ && dof == dof_ &&
               dofMrt == (dof_ && dofMrt_) && ssao == ssao_ && ssr == ssr_ && beforeGraph == beforeGraph_;
    }

    void Create(int w, int h, int samples, bool output_, bool hdr_, bool bloom_, bool fxaa_, bool dof_, bool dofMrt_, bool ssao_, bool ssr_, bool beforeGraph_) {
        Destroy();
        width = w;
        height = h;
        msaa = samples;
        withOutput = output_;
        hdr = hdr_;
        bloom = bloom_;
        beforeGraph = beforeGraph_;
        if (beforeGraph) {
            postColor = MakeAttachmentImage(w, h, kHdrFormat, 1, false, false, "oe-post-scene");
            postAtt = ColorView(postColor);
            postTex = TextureView(postColor);
        }
        fxaa = fxaa_;
        dof = dof_;
        ssr = ssr_;
        ssao = ssao_;
        if (ssao) {
            ssaoDepth = MakeAttachmentImage(w, h, kDofDepthFormat, 1, false, false, "oe-ssao-depth");
            ssaoDepthAtt = ColorView(ssaoDepth);
            ssaoDepthTex = TextureView(ssaoDepth);
        }
        if (ssr) {
            for (int i = 0; i < 3; ++i) {
                ssrSurface[i] = MakeAttachmentImage(w, h, kHdrFormat, 1, false, msaa > 1, "oe-ssr-surface");
                ssrAtt[i] = msaa > 1 ? ResolveView(ssrSurface[i]) : ColorView(ssrSurface[i]);
                ssrTex[i] = TextureView(ssrSurface[i]);
                if (msaa > 1) {
                    ssrMsaa[i] = MakeAttachmentImage(w, h, kHdrFormat, msaa, false, false, "oe-ssr-surface-msaa");
                    ssrMsaaAtt[i] = ColorView(ssrMsaa[i]);
                }
            }
            ssrColor = MakeAttachmentImage(w, h, kHdrFormat, 1, false, false, "oe-ssr-color");
            ssrColorAtt = ColorView(ssrColor); ssrColorTex = TextureView(ssrColor);
        }
        dofMrt = dof_ && dofMrt_;
        if (fxaa) {
            fxaaColor = MakeAttachmentImage(w, h, kColorFormat, 1, false, false, "oe-fxaa-source");
            fxaaAtt = ColorView(fxaaColor);
            fxaaTex = TextureView(fxaaColor);
        }
        const sg_pixel_format sceneFormat = hdr ? kHdrFormat : kColorFormat;
        if (msaa > 1) {
            sceneMsaa = MakeAttachmentImage(w, h, sceneFormat, msaa, false, false, "oe-scene-msaa");
            sceneMsaaAtt = ColorView(sceneMsaa);
            sceneColor = MakeAttachmentImage(w, h, sceneFormat, 1, false, true, "oe-scene");
            sceneColorAtt = ResolveView(sceneColor);
        } else {
            sceneColor = MakeAttachmentImage(w, h, sceneFormat, 1, false, false, "oe-scene");
            sceneColorAtt = ColorView(sceneColor);
        }
        sceneDepth = MakeAttachmentImage(w, h, kDepthFormat, msaa, true, false, "oe-scene-depth");
        sceneDepthAtt = DepthView(sceneDepth);
        sceneTex = TextureView(sceneColor);
        if (dofMrt && msaa > 1) {
            dofDepthMsaa = MakeAttachmentImage(w, h, kHdrFormat, msaa, false, false, "oe-view-depth-msaa");
            dofDepthMsaaAtt = ColorView(dofDepthMsaa);
            dofDepth = MakeAttachmentImage(w, h, kHdrFormat, 1, false, true, "oe-view-depth");
            dofDepthResolveAtt = ResolveView(dofDepth);
            dofDepthTex = TextureView(dofDepth);
        } else if (dofMrt) {
            dofDepth = MakeAttachmentImage(w, h, kHdrFormat, 1, false, false, "oe-view-depth");
            dofDepthAtt = ColorView(dofDepth);
            dofDepthTex = TextureView(dofDepth);
        } else if (dof) {
            dofDepth = MakeAttachmentImage(w, h, kDofDepthFormat, 1, false, false, "oe-dof-depth");
            dofDepthAtt = ColorView(dofDepth);
            dofDepthTex = TextureView(dofDepth);
        }
        if (dof) {
            dofHorizontal = MakeAttachmentImage(w, h, sceneFormat, 1, false, false, "oe-dof-horizontal");
            dofHorizontalAtt = ColorView(dofHorizontal);
            dofHorizontalTex = TextureView(dofHorizontal);
            dofVertical = MakeAttachmentImage(w, h, sceneFormat, 1, false, false, "oe-dof-vertical");
            dofVerticalAtt = ColorView(dofVertical);
            dofVerticalTex = TextureView(dofVertical);
        }
        if (bloom) {
            bloomHorizontal = MakeAttachmentImage(w, h, kHdrFormat, 1, false, false, "oe-bloom-horizontal");
            bloomHorizontalAtt = ColorView(bloomHorizontal);
            bloomHorizontalTex = TextureView(bloomHorizontal);
            bloomVertical = MakeAttachmentImage(w, h, kHdrFormat, 1, false, false, "oe-bloom-vertical");
            bloomVerticalAtt = ColorView(bloomVertical);
            bloomVerticalTex = TextureView(bloomVertical);
        }
        maskColor = MakeAttachmentImage(w, h, kColorFormat, 1, false, false, "oe-mask");
        maskAtt = ColorView(maskColor);
        maskTex = TextureView(maskColor);
        maskDepth = MakeAttachmentImage(w, h, kDepthFormat, 1, true, false, "oe-mask-depth");
        maskDepthAtt = DepthView(maskDepth);
        if (withOutput) {
            output = MakeAttachmentImage(w, h, kColorFormat, 1, false, false, "oe-output");
            outputAtt = ColorView(output);
            outputTex = TextureView(output);
        }
    }

    void Destroy() {
        if (width == 0) return;
        for (int i = 0; i < 3; ++i) {
            for (sg_view v : {ssrAtt[i], ssrMsaaAtt[i], ssrTex[i]}) if (v.id) sg_destroy_view(v);
            if (ssrSurface[i].id) sg_destroy_image(ssrSurface[i]);
            if (ssrMsaa[i].id) sg_destroy_image(ssrMsaa[i]);
        }
        if (ssrColorAtt.id) sg_destroy_view(ssrColorAtt);
        if (ssrColorTex.id) sg_destroy_view(ssrColorTex);
        if (ssrColor.id) sg_destroy_image(ssrColor);
        for (sg_view v : {sceneMsaaAtt, sceneDepthAtt, sceneColorAtt, sceneTex, maskAtt, maskDepthAtt, maskTex, outputAtt, outputTex,
                          bloomHorizontalAtt, bloomHorizontalTex, bloomVerticalAtt, bloomVerticalTex, fxaaAtt, fxaaTex, postAtt, postTex,
                          dofDepthAtt, dofDepthMsaaAtt, dofDepthResolveAtt, dofDepthTex, dofHorizontalAtt, dofHorizontalTex, dofVerticalAtt, dofVerticalTex, ssaoDepthAtt, ssaoDepthTex}) {
            if (v.id) sg_destroy_view(v);
        }
        for (sg_image i : {sceneMsaa, sceneDepth, sceneColor, maskColor, maskDepth, output, bloomHorizontal, bloomVertical, fxaaColor, postColor,
                           dofDepth, dofDepthMsaa, dofHorizontal, dofVertical, ssaoDepth}) {
            if (i.id) sg_destroy_image(i);
        }
        *this = Targets();
    }
};

void SgLog(const char* tag, uint32_t level, uint32_t item, const char* message, uint32_t line, const char*, void*) {
    // sokol emits a marker at warning level, then the actual compiler text at info level.
    // Preserve that diagnostic instead of dropping its text in Release logs.
    if (item == SG_LOGITEM_D3D11_SHADER_COMPILATION_OUTPUT) {
        if (message) OE_LOG_WARN("gpu", "D3D11 shader compiler: %s", message);
        return;
    }
    // GL drivers strip inputs a program never reads (vertex colour in a shader graph that ignores it, uniforms of
    // features a variant does not use); sokol then skips them. Expected, so only the name (sent at info level) is
    // kept, as debug.
    const char* unused = item == SG_LOGITEM_GL_VERTEX_ATTRIBUTE_NOT_FOUND_IN_SHADER ? "vertex attribute"
                       : item == SG_LOGITEM_GL_UNIFORMBLOCK_NAME_NOT_FOUND_IN_SHADER ? "uniform"
                       : item == SG_LOGITEM_GL_IMAGE_SAMPLER_NAME_NOT_FOUND_IN_SHADER ? "texture" : nullptr;
    if (unused) {
        if (message) OE_LOG_DEBUG("gpu", "GL program does not use %s '%s'", unused, message);
        return;
    }
    const char* msg = message;
    if (!msg) {
        switch (item) {
            case SG_LOGITEM_BUFFER_POOL_EXHAUSTED: msg = "BUFFER_POOL_EXHAUSTED: buffer pool exhausted"; break;
            case SG_LOGITEM_IMAGE_POOL_EXHAUSTED: msg = "IMAGE_POOL_EXHAUSTED: image pool exhausted"; break;
            case SG_LOGITEM_VIEW_POOL_EXHAUSTED: msg = "VIEW_POOL_EXHAUSTED: view pool exhausted"; break;
            case SG_LOGITEM_SAMPLER_POOL_EXHAUSTED: msg = "SAMPLER_POOL_EXHAUSTED: sampler pool exhausted"; break;
            case SG_LOGITEM_SHADER_POOL_EXHAUSTED: msg = "SHADER_POOL_EXHAUSTED: shader pool exhausted"; break;
            case SG_LOGITEM_PIPELINE_POOL_EXHAUSTED: msg = "PIPELINE_POOL_EXHAUSTED: pipeline pool exhausted"; break;
            default: msg = "(no message in release builds)"; break;
        }
    }
    if (level <= 1) OE_LOG_ERROR("gpu", "%s item %u line %u: %s", tag, item, line, msg);
    else if (level == 2) OE_LOG_WARN("gpu", "%s item %u line %u: %s", tag, item, line, msg);
}

}  // namespace

struct GpuRenderer::Impl {
    GpuDevice& device;
    AssetManager* assets;
    Settings settings;

    // Scene pass: [0 settings.msaa, 1 single-sampled (pixel-perfect art frames)]
    // [0 plain, 1 DOF view-depth, 2 SSR surface MRT][0 opaque, 1 alpha blend, 2 additive]
    // [faces: 0 front (back culled), 1 both (double sided), 2 back (front culled: outline shells)], see FaceMode
    sg_pipeline meshPips[2][3][3][3]{};
    sg_pipeline hdrMeshPips[2][3][3][3]{};
    sg_pipeline skyPips[2][2][3]{};  // [single][hdr][mrt]: fullscreen skybox background drawn first in the scene pass
    sg_shader skyShd{};
    sg_pipeline linePips[2][3]{}, hdrLinePips[2][3]{};
    sg_pipeline bloomPip{};
    sg_shader bloomShd{};
    int hdrSamples = 1;
    bool hdrSupported = false;
    sg_pipeline shadowPip{}, shadowPipTwoSided{}, maskDepthPip{}, maskDrawPip{}, compositePip{}, postScenePip{}, uiPip{};
    sg_shader meshShd{}, shadowShd{}, solidShd{}, lineShd{}, compositeShd{}, uiShd{};
    sg_shader surfaceAuxShd{};
    sg_pipeline surfaceShadow[2]{}, surfaceMask[2][2]{};
    // Depth of field: scene depth as color [double sided] (plain and cut-out surfaces), blur [0 RGBA8, 1 HDR].
    sg_shader sceneDepthShd{}, dofShd{}, ssrShd{};
    sg_pipeline ssrPip{};
    sg_pipeline sceneDepthPip[2]{}, surfaceDepthPip[2]{}, dofPip[2]{};
    bool dofSupported = false;
    bool dofMrtSupported = false;  // RGBA16F view depth beside the scene color at the scene pass's sample count
    sg_sampler linearRepeat{}, linearClamp{}, nearestClamp{}, pixelArt{}, shadowCompare{};
    sg_sampler envSampler{};  // environment images: around the horizon it repeats, at the poles it clamps
    bool envHalfFloat = false;  // RGBA16F textures can be filtered here (else environments are stored as RGBA8)
    sg_image white{}, shadowMap{};
    sg_view whiteTex{}, shadowAtt{}, shadowTex{};
    sg_image cascadeAtlas{};  // Lazy 2048-square depth image; four 1024-square tiles, one texture slot.
    sg_view cascadeAtt{}, cascadeTex{};
    sg_image localShadowAtlas{};  // Two local lights, up to twelve perspective maps in 512-square tiles.
    sg_view localShadowAtt{}, localShadowTex{};
    sg_image spotImage{};  // Small immutable light-data image, replaced only when the light values change.
    sg_view spotTex{};
    float spotData[kLocalLightDataWidth * kLocalLightDataHeight * 4]{};
    StreamBuffer lines, ui, batchVertices, batchIndices{sg_buffer{}, 0, true};
    Targets offscreen, window, planarTarget, pixelSource;
    DecalAtlas decalAtlas;
    std::unordered_map<int, Targets> panelTargets;  // RenderToTexture slots (editor panels)

    // Shader graphs compiled into their own mesh/surface programs (GraphSource.h); pipelines are made
    // on first use from the descriptions of the standard ones. A graph whose program fails to compile
    // keeps the interpreter (the standard pipelines).
    struct GraphProgram {
        std::weak_ptr<const ShaderGraph> owner;
        sg_shader mesh{}, aux{};
        bool failed = false;
        std::unordered_map<uint32_t, sg_pipeline> pipelines;  // standard pipeline id -> this graph's
        std::future<std::pair<ProgramCode, ProgramCode>> compiling;  // mesh + aux on a worker (asyncShaders)
        void Destroy() {
            for (auto& kv : pipelines) sg_destroy_pipeline(kv.second);
            if (mesh.id) sg_destroy_shader(mesh);
            if (aux.id) sg_destroy_shader(aux);
        }
    };
    std::unordered_map<const ShaderGraph*, GraphProgram> graphPrograms;
    // Post graphs (PostProcess.shader) compiled into their own composite program (GraphPostBody in place
    // of the interpreter loop of PostGraph). Until it is ready, or when it fails, the interpreter runs it
    // (graphs up to kMaxInterpretedPostNodes; larger ones are skipped).
    struct PostProgram {
        std::weak_ptr<const ShaderGraph> owner;
        sg_shader shader{};
        sg_pipeline pipeline{};
        bool failed = false;
        std::future<ProgramCode> compiling;
        void Destroy() {
            if (pipeline.id) sg_destroy_pipeline(pipeline);
            if (shader.id) sg_destroy_shader(shader);
        }
    };
    std::unordered_map<const ShaderGraph*, PostProgram> postPrograms;
    sg_pipeline_desc compositeDesc{};
    std::string compiler;  // device.ShaderCompilerId(); empty = sokol_gfx compiles source
    std::unordered_map<uint32_t, sg_pipeline_desc> graphPipelineDescs;  // standard mesh/surface pipelines by id

    std::unordered_map<const Mesh*, GpuMesh> meshes;
    std::unordered_map<const Mesh*, GpuMesh> flatMeshes;
    std::unordered_map<const Mesh*, GpuMesh> smoothMeshes;  // Material::smoothNormals
    Vec4 graphViewer;  // the camera of the frame being drawn, for the viewDirection graph input (ShaderViewer)
    bool meshesLinear = false;  // the mesh buffers hold decoded vertex colors (RenderLights::linear)
    std::unordered_map<const Texture*, GpuTexture> textures;
    std::unordered_map<const Texture*, GpuTexture> srgbTextures;  // color textures of linear projects (SRGB8A8)
    std::unordered_map<const EnvironmentMap*, GpuEnvironment> environments;
    std::unordered_map<const Texture*, GpuTexture> uiTextures;  // no mips; re-uploaded when Texture::version changes

    Impl(GpuDevice& d, AssetManager* a, const Settings& s) : device(d), assets(a), settings(s) {}

    void Init() {
        sg_backend backend = sg_query_backend();
        compiler = device.ShaderCompilerId();
        TrimShaderCache(settings.shaderCacheDir, 400);
        sg_shader* const shaders[] = {&meshShd, &surfaceAuxShd, &shadowShd, &solidShd, &lineShd,
                                      &compositeShd, &uiShd, &bloomShd, &sceneDepthShd, &dofShd, &skyShd, &ssrShd};
        const sg_shader_desc* const descs[] = {
            oe_mesh_shader_desc(backend), oe_surface_aux_shader_desc(backend), oe_shadow_shader_desc(backend),
            oe_solid_shader_desc(backend), oe_line_shader_desc(backend), oe_composite_shader_desc(backend),
            oe_ui_shader_desc(backend), oe_bloom_shader_desc(backend), oe_scene_depth_shader_desc(backend),
            oe_dof_shader_desc(backend), oe_sky_shader_desc(backend), oe_ssr_shader_desc(backend)};
        // Where the device compiles source itself the programs compile in parallel or come from the
        // cache; otherwise sokol_gfx takes the source as it is.
        std::vector<std::future<ProgramCode>> codes;
        if (!compiler.empty()) {
            for (const sg_shader_desc* desc : descs) {
                codes.push_back(std::async(std::launch::async, [this, desc] {
                    ProgramCode code{StageOf(desc->vertex_func, "vs_4_0"), StageOf(desc->fragment_func, "ps_4_0")};
                    CompileProgram(device, compiler, settings.shaderCacheDir, code);
                    return code;
                }));
            }
        }
        for (size_t i = 0; i < std::size(descs); ++i) {
            const ProgramCode code = codes.empty() ? ProgramCode() : codes[i].get();
            *shaders[i] = code.ok ? MakeCompiledShader(*descs[i], code) : sg_make_shader(descs[i]);
        }

        const int msaa = settings.msaa;
        const sg_pixelformat_info hdrInfo = sg_query_pixelformat(kHdrFormat);
        hdrSupported = hdrInfo.render && hdrInfo.blend && hdrInfo.filter;
        hdrSamples = hdrInfo.msaa ? msaa : 1;
        dofSupported = sg_query_pixelformat(kDofDepthFormat).render;
        // Every attachment of a pass shares the sample count: RGBA8 scenes use settings.msaa, HDR scenes hdrSamples.
        dofMrtSupported = hdrSupported && (msaa == 1 || hdrInfo.msaa);
        if (hdrSupported) {
            sg_pipeline_desc d{};
            d.shader = bloomShd;
            d.depth.pixel_format = SG_PIXELFORMAT_NONE;
            d.colors[0].pixel_format = kHdrFormat;
            d.sample_count = 1;
            d.label = "oe-bloom";
            bloomPip = sg_make_pipeline(&d);
        }
        auto alphaBlend = [](sg_color_target_state& c) {
            c.pixel_format = kColorFormat;
            c.blend.enabled = true;
            c.blend.src_factor_rgb = SG_BLENDFACTOR_SRC_ALPHA;
            c.blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            // Alpha composites "over" too, so an opaque target stays opaque. Replacing it
            // (ONE, ZERO) left each UI glyph quad's coverage in the output alpha, which
            // anything drawing the image with blending (the native editor) showed as dark boxes.
            c.blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
            c.blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        };
        // The second color target of the scene pass (depth of field): view depth of opaque/cut-out surfaces only.
        auto viewDepthTarget = [](sg_pipeline_desc& d, int mrt, bool write) {
            if (!mrt) return;
            d.color_count = 2;
            d.colors[1].pixel_format = kHdrFormat;
            d.colors[1].write_mask = write ? SG_COLORMASK_R : SG_COLORMASK_NONE;
            if (mrt == 2) {
                d.color_count = 4;
                for (int i = 1; i < 4; ++i) {
                    d.colors[i].pixel_format = kHdrFormat;
                    d.colors[i].write_mask = write ? SG_COLORMASK_RGBA : SG_COLORMASK_NONE;
                }
            }
        };
        // Opaque/cut-out surfaces write depth; transparent ones blend and only test it. Pixel-perfect
        // frames draw single-sampled (single = 1): a cut edge inside an art pixel must not blend.
        for (int single = 0; single < 2; ++single)
        for (int mrt = 0; mrt < 3; ++mrt)
        for (int blend = 0; blend < 3; ++blend) {
            for (int twoSided = 0; twoSided < 3; ++twoSided) {
                if ((mrt == 1 && !dofMrtSupported) || (mrt == 2 && !hdrSupported)) continue;
                if (single && msaa == 1) {
                    meshPips[1][mrt][blend][twoSided] = meshPips[0][mrt][blend][twoSided];
                    hdrMeshPips[1][mrt][blend][twoSided] = hdrMeshPips[0][mrt][blend][twoSided];
                    continue;
                }
                sg_pipeline_desc d{};
                d.shader = meshShd;
                d.layout.buffers[0].stride = sizeof(MeshVertex);
                d.layout.attrs[ATTR_oe_mesh_position] = {0, offsetof(MeshVertex, pos), SG_VERTEXFORMAT_FLOAT3};
                d.layout.attrs[ATTR_oe_mesh_normal] = {0, offsetof(MeshVertex, nrm), SG_VERTEXFORMAT_FLOAT3};
                d.layout.attrs[ATTR_oe_mesh_texcoord] = {0, offsetof(MeshVertex, uv), SG_VERTEXFORMAT_FLOAT2};
                d.layout.attrs[ATTR_oe_mesh_tangent] = {0, offsetof(MeshVertex, tan), SG_VERTEXFORMAT_FLOAT4};
                d.layout.attrs[ATTR_oe_mesh_joint_indices] = {0, offsetof(MeshVertex, joints), SG_VERTEXFORMAT_FLOAT4};
                d.layout.attrs[ATTR_oe_mesh_joint_weights] = {0, offsetof(MeshVertex, weights), SG_VERTEXFORMAT_FLOAT4};
            d.layout.attrs[ATTR_oe_mesh_color0] = {0, offsetof(MeshVertex, color), SG_VERTEXFORMAT_FLOAT4};
            d.layout.attrs[ATTR_oe_mesh_particle0] = {0, offsetof(MeshVertex, particle), SG_VERTEXFORMAT_FLOAT4};
            d.layout.attrs[ATTR_oe_mesh_custom0] = {0, offsetof(MeshVertex, custom), SG_VERTEXFORMAT_FLOAT4};
                d.index_type = SG_INDEXTYPE_UINT32;
                d.depth.pixel_format = kDepthFormat;
                d.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
                d.depth.write_enabled = blend == 0;
                d.colors[0].pixel_format = kColorFormat;
                if (blend) alphaBlend(d.colors[0]);
                if (blend == 2) {  // additive: color * alpha on top, target alpha untouched
                    d.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE;
                    d.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ZERO;
                    d.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;
                }
                d.cull_mode = twoSided == 2 ? SG_CULLMODE_FRONT : twoSided ? SG_CULLMODE_NONE : SG_CULLMODE_BACK;
                d.face_winding = SG_FACEWINDING_CCW;
                d.sample_count = single ? 1 : msaa;
                viewDepthTarget(d, mrt, blend == 0);
                if (mrt == 2 && blend == 1) {
                    // A transparent foreground attenuates the underlying SSR correction, without becoming a receiver.
                    for (int i = 2; i < 4; ++i) {
                        alphaBlend(d.colors[i]); d.colors[i].write_mask = SG_COLORMASK_RGB;
                    }
                }
                d.label = blend ? "oe-mesh-blend" : "oe-mesh";
                if (mrt != 2) meshPips[single][mrt][blend][twoSided] = GraphCapablePipeline(d);
                if (hdrSupported) {
                    d.colors[0].pixel_format = kHdrFormat;
                    d.sample_count = single ? 1 : hdrSamples;
                    d.label = blend ? "oe-hdr-mesh-blend" : "oe-hdr-mesh";
                    hdrMeshPips[single][mrt][blend][twoSided] = GraphCapablePipeline(d);
                }
            }
        }
        // Skybox background: a fullscreen triangle (no vertex buffer) drawn first in the scene pass,
        // depth test always/no write so every later surface draws over it. One variant per scene
        // color format (RGBA8 / HDR) and per depth-of-field MRT state.
        for (int single = 0; single < 2; ++single)
        for (int hdr = 0; hdr < 2; ++hdr) {
            if (hdr && !hdrSupported) continue;
            for (int mrt = 0; mrt < 3; ++mrt) {
                if (mrt == 2 && !hdr) continue;
                if ((mrt == 1 && !dofMrtSupported) || (mrt == 2 && !hdrSupported)) continue;
                if (single && msaa == 1) {
                    skyPips[1][hdr][mrt] = skyPips[0][hdr][mrt];
                    continue;
                }
                sg_pipeline_desc d{};
                d.shader = skyShd;
                d.depth.pixel_format = kDepthFormat;
                d.depth.compare = SG_COMPAREFUNC_ALWAYS;
                d.depth.write_enabled = false;
                d.cull_mode = SG_CULLMODE_NONE;
                d.colors[0].pixel_format = hdr ? kHdrFormat : kColorFormat;
                d.sample_count = single ? 1 : (hdr ? hdrSamples : msaa);
                viewDepthTarget(d, mrt, false);  // leave the depth-of-field target at its cleared far value
                d.label = "oe-sky";
                skyPips[single][hdr][mrt] = sg_make_pipeline(&d);
            }
        }

        if (hdrSupported && dofSupported) {
            sg_pipeline_desc d{};
            d.shader = ssrShd; d.depth.pixel_format = SG_PIXELFORMAT_NONE;
            d.colors[0].pixel_format = kHdrFormat; d.sample_count = 1;
            d.label = "oe-ssr"; ssrPip = sg_make_pipeline(&d);
        }

        // Shadow and selection passes use the same skin influences as the scene.
        auto positionOnly = [](sg_pipeline_desc& d) {
            d.layout.buffers[0].stride = sizeof(MeshVertex);
            d.layout.attrs[0] = {0, 0, SG_VERTEXFORMAT_FLOAT3};
            d.layout.attrs[ATTR_oe_shadow_joint_indices] = {0, offsetof(MeshVertex, joints), SG_VERTEXFORMAT_FLOAT4};
            d.layout.attrs[ATTR_oe_shadow_joint_weights] = {0, offsetof(MeshVertex, weights), SG_VERTEXFORMAT_FLOAT4};
            d.index_type = SG_INDEXTYPE_UINT32;
            d.face_winding = SG_FACEWINDING_CCW;
            d.depth.pixel_format = kDepthFormat;
            d.sample_count = 1;
        };
        {
            sg_pipeline_desc d{};
            positionOnly(d);
            d.shader = shadowShd;
            d.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
            d.depth.write_enabled = true;
            d.colors[0].pixel_format = SG_PIXELFORMAT_NONE;  // depth only
            // Back faces into the shadow map: avoids self-shadowing acne on lit faces.
            d.cull_mode = SG_CULLMODE_FRONT;
            d.label = "oe-shadow";
            shadowPip = sg_make_pipeline(&d);
            d.cull_mode = SG_CULLMODE_NONE;  // double-sided materials
            d.label = "oe-shadow-two-sided";
            shadowPipTwoSided = sg_make_pipeline(&d);
        }
        {
            sg_pipeline_desc d{};
            positionOnly(d);
            d.shader = solidShd;
            d.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
            d.depth.write_enabled = true;
            d.colors[0].pixel_format = kColorFormat;
            d.colors[0].write_mask = SG_COLORMASK_NONE;
            d.cull_mode = SG_CULLMODE_BACK;
            d.label = "oe-mask-depth";
            maskDepthPip = sg_make_pipeline(&d);
            d.depth.write_enabled = false;
            d.colors[0].write_mask = SG_COLORMASK_RGBA;
            d.label = "oe-mask-draw";
            maskDrawPip = sg_make_pipeline(&d);
        }
        for (int twoSided = 0; twoSided < 2; ++twoSided) {
            sg_pipeline_desc d{};
            d.shader = surfaceAuxShd;
            d.layout.buffers[0].stride = sizeof(MeshVertex);
            d.layout.attrs[ATTR_oe_surface_aux_position] = {0, offsetof(MeshVertex, pos), SG_VERTEXFORMAT_FLOAT3};
            d.layout.attrs[ATTR_oe_surface_aux_normal] = {0, offsetof(MeshVertex, nrm), SG_VERTEXFORMAT_FLOAT3};
            d.layout.attrs[ATTR_oe_surface_aux_texcoord] = {0, offsetof(MeshVertex, uv), SG_VERTEXFORMAT_FLOAT2};
            d.layout.attrs[ATTR_oe_surface_aux_tangent] = {0, offsetof(MeshVertex, tan), SG_VERTEXFORMAT_FLOAT4};
            d.layout.attrs[ATTR_oe_surface_aux_joint_indices] = {0, offsetof(MeshVertex, joints), SG_VERTEXFORMAT_FLOAT4};
            d.layout.attrs[ATTR_oe_surface_aux_joint_weights] = {0, offsetof(MeshVertex, weights), SG_VERTEXFORMAT_FLOAT4};
            d.layout.attrs[ATTR_oe_surface_aux_color0] = {0, offsetof(MeshVertex, color), SG_VERTEXFORMAT_FLOAT4};
            d.layout.attrs[ATTR_oe_surface_aux_particle0] = {0, offsetof(MeshVertex, particle), SG_VERTEXFORMAT_FLOAT4};
            d.layout.attrs[ATTR_oe_surface_aux_custom0] = {0, offsetof(MeshVertex, custom), SG_VERTEXFORMAT_FLOAT4};
            d.index_type = SG_INDEXTYPE_UINT32;
            d.face_winding = SG_FACEWINDING_CCW;
            d.depth.pixel_format = kDepthFormat;
            d.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
            d.depth.write_enabled = true;
            d.sample_count = 1;
            d.colors[0].pixel_format = SG_PIXELFORMAT_NONE;
            d.cull_mode = twoSided ? SG_CULLMODE_NONE : SG_CULLMODE_FRONT;
            d.label = "oe-surface-shadow";
            surfaceShadow[twoSided] = GraphCapablePipeline(d);
            if (dofSupported) {
                d.colors[0].pixel_format = kDofDepthFormat;
                d.cull_mode = twoSided ? SG_CULLMODE_NONE : SG_CULLMODE_BACK;
                d.label = "oe-surface-depth";
                surfaceDepthPip[twoSided] = GraphCapablePipeline(d);
            }
            d.colors[0].pixel_format = kColorFormat;
            d.cull_mode = twoSided ? SG_CULLMODE_NONE : SG_CULLMODE_BACK;
            for (int stage = 0; stage < 2; ++stage) {
                d.depth.write_enabled = stage == 0;
                d.colors[0].write_mask = stage == 0 ? SG_COLORMASK_NONE : SG_COLORMASK_RGBA;
                d.label = "oe-surface-mask";
                surfaceMask[stage][twoSided] = GraphCapablePipeline(d);
            }
        }
        if (dofSupported) {
            for (int twoSided = 0; twoSided < 2; ++twoSided) {
                sg_pipeline_desc d{};
                positionOnly(d);
                d.shader = sceneDepthShd;
                d.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
                d.depth.write_enabled = true;
                d.colors[0].pixel_format = kDofDepthFormat;
                d.cull_mode = twoSided ? SG_CULLMODE_NONE : SG_CULLMODE_BACK;
                d.label = "oe-scene-depth";
                sceneDepthPip[twoSided] = sg_make_pipeline(&d);
            }
            for (int hdr = 0; hdr < 2; ++hdr) {
                if (hdr && !hdrSupported) continue;
                sg_pipeline_desc d{};
                d.shader = dofShd;
                d.depth.pixel_format = SG_PIXELFORMAT_NONE;
                d.colors[0].pixel_format = hdr ? kHdrFormat : kColorFormat;
                d.sample_count = 1;
                d.label = "oe-dof";
                dofPip[hdr] = sg_make_pipeline(&d);
            }
        }
        for (int single = 0; single < 2; ++single)
        for (int mrt = 0; mrt < 3; ++mrt) {
            if ((mrt == 1 && !dofMrtSupported) || (mrt == 2 && !hdrSupported)) continue;
            if (single && msaa == 1) {
                linePips[1][mrt] = linePips[0][mrt];
                hdrLinePips[1][mrt] = hdrLinePips[0][mrt];
                continue;
            }
            sg_pipeline_desc d{};
            d.shader = lineShd;
            d.layout.buffers[0].stride = sizeof(ColorVertex);
            d.layout.attrs[ATTR_oe_line_position] = {0, offsetof(ColorVertex, pos), SG_VERTEXFORMAT_FLOAT3};
            d.layout.attrs[ATTR_oe_line_color0] = {0, offsetof(ColorVertex, color), SG_VERTEXFORMAT_FLOAT4};
            d.primitive_type = SG_PRIMITIVETYPE_LINES;
            d.depth.pixel_format = kDepthFormat;
            d.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
            alphaBlend(d.colors[0]);
            d.sample_count = single ? 1 : msaa;
            viewDepthTarget(d, mrt, false);
            d.label = "oe-lines";
            if (mrt != 2) linePips[single][mrt] = sg_make_pipeline(&d);
            if (hdrSupported) {
                d.colors[0].pixel_format = kHdrFormat;
                d.sample_count = single ? 1 : hdrSamples;
                d.label = "oe-hdr-lines";
                hdrLinePips[single][mrt] = sg_make_pipeline(&d);
            }
        }
        {
            sg_pipeline_desc d{};
            d.shader = compositeShd;
            d.depth.pixel_format = SG_PIXELFORMAT_NONE;
            d.colors[0].pixel_format = kColorFormat;
            d.sample_count = 1;
            d.label = "oe-composite";
            compositePip = sg_make_pipeline(&d);
            compositeDesc = d;  // compiled post graphs get the same pipeline with their own program
            d.colors[0].pixel_format = kHdrFormat;
            d.label = "oe-post-scene";
            postScenePip = sg_make_pipeline(&d);
        }
        {
            sg_pipeline_desc d{};
            d.shader = uiShd;
            d.layout.buffers[0].stride = sizeof(UIVertex);
            d.layout.attrs[ATTR_oe_ui_position] = {0, offsetof(UIVertex, pos), SG_VERTEXFORMAT_FLOAT2};
            d.layout.attrs[ATTR_oe_ui_texcoord0] = {0, offsetof(UIVertex, uv), SG_VERTEXFORMAT_FLOAT2};
            d.layout.attrs[ATTR_oe_ui_color0] = {0, offsetof(UIVertex, color), SG_VERTEXFORMAT_FLOAT4};
            d.layout.attrs[ATTR_oe_ui_color1] = {0, offsetof(UIVertex, border), SG_VERTEXFORMAT_FLOAT4};
            d.layout.attrs[ATTR_oe_ui_shape_rect] = {0, offsetof(UIVertex, rect), SG_VERTEXFORMAT_FLOAT4};
            d.layout.attrs[ATTR_oe_ui_shape_params] = {0, offsetof(UIVertex, params), SG_VERTEXFORMAT_FLOAT4};
            d.depth.pixel_format = SG_PIXELFORMAT_NONE;
            alphaBlend(d.colors[0]);
            d.sample_count = 1;
            d.label = "oe-ui";
            uiPip = sg_make_pipeline(&d);
        }

        {
            sg_sampler_desc d{};
            d.min_filter = SG_FILTER_LINEAR;
            d.mag_filter = SG_FILTER_LINEAR;
            d.mipmap_filter = SG_FILTER_LINEAR;
            d.wrap_u = SG_WRAP_REPEAT;
            d.wrap_v = SG_WRAP_REPEAT;
            d.max_anisotropy = 8;
            linearRepeat = sg_make_sampler(&d);
            d = sg_sampler_desc{};
            d.min_filter = SG_FILTER_LINEAR;
            d.mag_filter = SG_FILTER_LINEAR;
            d.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
            d.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
            linearClamp = sg_make_sampler(&d);
            d = sg_sampler_desc{};
            d.min_filter = SG_FILTER_NEAREST;
            d.mag_filter = SG_FILTER_NEAREST;
            d.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
            d.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
            nearestClamp = sg_make_sampler(&d);
            // Pixel art (sprites/tilemaps): sharp texels, no mip blending across sheet frames.
            d = sg_sampler_desc{};
            d.min_filter = SG_FILTER_NEAREST;
            d.mag_filter = SG_FILTER_NEAREST;
            d.mipmap_filter = SG_FILTER_NEAREST;
            d.max_lod = 0.0f;
            d.wrap_u = SG_WRAP_REPEAT;
            d.wrap_v = SG_WRAP_REPEAT;
            pixelArt = sg_make_sampler(&d);
            d = sg_sampler_desc{};
            d.min_filter = SG_FILTER_LINEAR;
            d.mag_filter = SG_FILTER_LINEAR;
            d.wrap_u = SG_WRAP_CLAMP_TO_EDGE;
            d.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
            d.compare = SG_COMPAREFUNC_LESS_EQUAL;
            shadowCompare = sg_make_sampler(&d);
            d = sg_sampler_desc{};
            d.min_filter = SG_FILTER_LINEAR;
            d.mag_filter = SG_FILTER_LINEAR;
            d.mipmap_filter = SG_FILTER_LINEAR;
            d.wrap_u = SG_WRAP_REPEAT;
            d.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
            envSampler = sg_make_sampler(&d);
            envHalfFloat = sg_query_pixelformat(kHdrFormat).filter;
        }
        {
            static const uint32_t kWhite[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
            sg_image_desc d{};
            d.width = 2;
            d.height = 2;
            d.pixel_format = kColorFormat;
            d.data.mip_levels[0] = {kWhite, sizeof(kWhite)};
            d.label = "oe-white";
            white = sg_make_image(&d);
            whiteTex = TextureView(white);
        }
        shadowMap = MakeAttachmentImage(settings.shadowMapSize, settings.shadowMapSize, kDepthFormat, 1, true, false, "oe-shadow-map");
        shadowAtt = DepthView(shadowMap);
        shadowTex = TextureView(shadowMap);
    }

    // A standard pipeline that graph materials get their own copy of (GraphPipeline).
    sg_pipeline GraphCapablePipeline(const sg_pipeline_desc& d) {
        sg_pipeline pipeline = sg_make_pipeline(&d);
        graphPipelineDescs[pipeline.id] = d;
        return pipeline;
    }

    // `standard` with the material's compiled graph program, or `standard` itself (no graph, failed).
    sg_pipeline GraphPipeline(sg_pipeline standard, const Material& material) {
        if (!material.shader || !settings.compileGraphs) return standard;
        GraphProgram& program = ProgramFor(material.shader);
        if (program.compiling.valid()) {
            if (program.compiling.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return standard;
            const std::pair<ProgramCode, ProgramCode> codes = program.compiling.get();
            const sg_backend backend = sg_query_backend();
            const bool ok = codes.first.ok && codes.second.ok;
            FinishGraph(program, ok ? MakeGraphShader(*oe_mesh_shader_desc(backend), codes.first) : sg_shader{},
                        ok ? MakeGraphShader(*oe_surface_aux_shader_desc(backend), codes.second) : sg_shader{});
            OE_LOG_DEBUG("gpu", "shader graph program ready (compiled in the background)");
        }
        if (program.failed) return standard;
        auto found = program.pipelines.find(standard.id);
        if (found != program.pipelines.end()) return found->second;
        sg_pipeline_desc d = graphPipelineDescs.at(standard.id);
        d.shader = d.shader.id == meshShd.id ? program.mesh : program.aux;
        d.label = "oe-graph";
        return program.pipelines[standard.id] = sg_make_pipeline(&d);
    }

    GraphProgram& ProgramFor(const std::shared_ptr<const ShaderGraph>& graph) {
        auto found = graphPrograms.find(graph.get());
        if (found != graphPrograms.end() && !found->second.owner.expired()) return found->second;
        if (found != graphPrograms.end()) {
            found->second.Destroy();
            graphPrograms.erase(found);
        }
        GraphProgram& program = graphPrograms[graph.get()];
        program.owner = graph;
        const sg_backend backend = sg_query_backend();
        ProgramCode mesh = GraphCode(*oe_mesh_shader_desc(backend), *graph);
        ProgramCode aux = GraphCode(*oe_surface_aux_shader_desc(backend), *graph);
        if (compiler.empty()) {  // sokol_gfx compiles the source
            const sg_shader meshShader = MakeGraphShader(*oe_mesh_shader_desc(backend), mesh);
            FinishGraph(program, meshShader, meshShader.id ? MakeGraphShader(*oe_surface_aux_shader_desc(backend), aux) : sg_shader{});
        } else if (settings.asyncShaders) {  // GraphPipeline picks the result up once it is ready
            program.compiling = std::async(std::launch::async, [this, mesh, aux]() mutable {
                if (CompileProgram(device, compiler, settings.shaderCacheDir, mesh))
                    CompileProgram(device, compiler, settings.shaderCacheDir, aux);
                return std::make_pair(std::move(mesh), std::move(aux));
            });
        } else {
            const bool ok = CompileProgram(device, compiler, settings.shaderCacheDir, mesh) &&
                            CompileProgram(device, compiler, settings.shaderCacheDir, aux);
            FinishGraph(program, ok ? MakeGraphShader(*oe_mesh_shader_desc(backend), mesh) : sg_shader{},
                        ok ? MakeGraphShader(*oe_surface_aux_shader_desc(backend), aux) : sg_shader{});
        }
        return program;
    }

    // The composite pipeline running `graph` compiled, or none while it compiles or when it failed (the
    // standard composite pipeline interprets it then).
    sg_pipeline PostPipeline(const std::shared_ptr<const ShaderGraph>& graph) {
        if (!settings.compileGraphs) return sg_pipeline{};
        auto found = postPrograms.find(graph.get());
        if (found != postPrograms.end() && found->second.owner.expired()) {
            found->second.Destroy();
            postPrograms.erase(found);
            found = postPrograms.end();
        }
        const sg_shader_desc& standard = *oe_composite_shader_desc(sg_query_backend());
        if (found == postPrograms.end()) {
            PostProgram& program = postPrograms[graph.get()];
            program.owner = graph;
            ProgramCode code{StageOf(standard.vertex_func, "vs_4_0"), StageOf(standard.fragment_func, "ps_4_0")};
            const GraphDialect dialect = sg_query_backend() == SG_BACKEND_D3D11 ? GraphDialect::Hlsl : GraphDialect::Glsl;
            if (code.vs.source.empty() || code.fs.source.empty() ||
                !ReplaceFunctionBody(code.fs.source, "PostGraph", GraphPostBody(*graph, dialect))) {
                program.failed = true;
                OE_LOG_WARN("gpu", "post graph: no PostGraph source to replace; it is interpreted instead");
                return sg_pipeline{};
            }
            if (compiler.empty()) {  // sokol_gfx compiles the source
                FinishPost(program, MakeGraphShader(standard, code));
            } else if (settings.asyncShaders) {  // picked up below once it is ready
                program.compiling = std::async(std::launch::async, [this, code]() mutable {
                    CompileProgram(device, compiler, settings.shaderCacheDir, code);
                    return code;
                });
            } else {
                CompileProgram(device, compiler, settings.shaderCacheDir, code);
                FinishPost(program, MakeGraphShader(standard, code));
            }
            found = postPrograms.find(graph.get());
        }
        PostProgram& program = found->second;
        if (program.compiling.valid()) {
            if (program.compiling.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return sg_pipeline{};
            const ProgramCode code = program.compiling.get();
            FinishPost(program, MakeGraphShader(standard, code));
            OE_LOG_DEBUG("gpu", "post graph program ready (compiled in the background)");
        }
        return program.failed ? sg_pipeline{} : program.pipeline;
    }

    void FinishPost(PostProgram& program, sg_shader shader) {
        program.shader = shader;
        program.failed = !shader.id;
        if (program.failed) {
            OE_LOG_WARN("gpu", "a post graph did not compile for the GPU; it is interpreted instead");
            return;
        }
        sg_pipeline_desc d = compositeDesc;
        d.shader = shader;
        if (const auto owner = program.owner.lock())
            if (owner->beforeToneMapping) d.colors[0].pixel_format = kHdrFormat;
        d.label = "oe-post-graph";
        program.pipeline = sg_make_pipeline(&d);
    }

    static void FinishGraph(GraphProgram& program, sg_shader mesh, sg_shader aux) {
        program.mesh = mesh;
        program.aux = aux;
        program.failed = !mesh.id || !aux.id;
        if (program.failed) OE_LOG_WARN("gpu", "a shader graph did not compile for the GPU; it is interpreted instead");
    }

    // The program `standard` with the graph in place of the interpreter loops, as source (none
    // when `standard` has no source to patch).
    static ProgramCode GraphCode(const sg_shader_desc& standard, const ShaderGraph& graph) {
        ProgramCode code{StageOf(standard.vertex_func, "vs_4_0"), StageOf(standard.fragment_func, "ps_4_0")};
        const GraphDialect dialect = sg_query_backend() == SG_BACKEND_D3D11 ? GraphDialect::Hlsl : GraphDialect::Glsl;
        if (code.vs.source.empty() || code.fs.source.empty() ||
            !ReplaceFunctionBody(code.vs.source, "VertexOffset", GraphVertexBody(graph, dialect)) ||
            !ReplaceFunctionBody(code.fs.source, "SurfaceGraph", GraphSurfaceBody(graph, dialect))) {
            code.vs.source.clear();
            code.fs.source.clear();
            return code;
        }
        // The light stage lives in programs that shade (mesh_fs calls LightGraph); the others have no LightGraph.
        const bool shades = code.fs.source.find("LightGraph(") != std::string::npos;
        if (shades && !ReplaceFunctionBody(code.fs.source, "LightGraph", GraphLightBody(graph, dialect))) {
            code.vs.source.clear();
            code.fs.source.clear();
        }
        return code;
    }

    // A graph program from compiled stages (code.ok) or else from its source; none when it fails.
    static sg_shader MakeGraphShader(sg_shader_desc desc, const ProgramCode& code) {
        if (code.vs.source.empty() || code.fs.source.empty()) return sg_shader{};
        desc.label = "oe-graph";
        sg_shader shader;
        if (code.ok) {
            shader = MakeCompiledShader(desc, code);
        } else {
            desc.vertex_func.source = code.vs.source.c_str();
            desc.fragment_func.source = code.fs.source.c_str();
            shader = sg_make_shader(&desc);
        }
        if (sg_query_shader_state(shader) == SG_RESOURCESTATE_VALID) return shader;
        sg_destroy_shader(shader);
        return sg_shader{};
    }

    void Shutdown() {
        if (localShadowTex.id) sg_destroy_view(localShadowTex);
        if (localShadowAtt.id) sg_destroy_view(localShadowAtt);
        if (localShadowAtlas.id) sg_destroy_image(localShadowAtlas);
        if (cascadeTex.id) sg_destroy_view(cascadeTex);
        if (cascadeAtt.id) sg_destroy_view(cascadeAtt);
        if (cascadeAtlas.id) sg_destroy_image(cascadeAtlas);
        if (spotTex.id) sg_destroy_view(spotTex);
        if (spotImage.id) sg_destroy_image(spotImage);
        for (auto& kv : graphPrograms) kv.second.Destroy();
        graphPrograms.clear();
        for (auto& kv : postPrograms) {
            if (kv.second.compiling.valid()) kv.second.compiling.wait();  // the worker holds `this`
            kv.second.Destroy();
        }
        postPrograms.clear();
        for (auto& kv : meshes) kv.second.Destroy();
        for (auto& kv : flatMeshes) kv.second.Destroy();
        for (auto& kv : smoothMeshes) kv.second.Destroy();
        for (auto& kv : textures) kv.second.Destroy();
        for (auto& kv : srgbTextures) kv.second.Destroy();
        for (auto& kv : uiTextures) kv.second.Destroy();
        meshes.clear();
        flatMeshes.clear();
        smoothMeshes.clear();
        textures.clear();
        srgbTextures.clear();
        uiTextures.clear();
        offscreen.Destroy();
        planarTarget.Destroy();
        window.Destroy();
        pixelSource.Destroy();
        for (auto& kv : panelTargets) kv.second.Destroy();
        panelTargets.clear();
        lines.Destroy();
        ui.Destroy();
        batchVertices.Destroy();
        batchIndices.Destroy();
    }

    // single: one sample per pixel whatever settings.msaa says (pixel-perfect art frames).
    void PrepareTargets(Targets& t, int w, int h, bool output, const RenderView& view, bool single = false) {
        const PostProcess post = NormalizePostProcess(view.postProcess);
        // Linear lighting keeps the scene in float and encodes it once, in the composite.
        const PostGraph graph = ResolvePostGraph(post, assets);
        const bool beforeGraph = graph.graph && graph.graph->beforeToneMapping;
        const bool hdr = UsesHdr(post) || (assets && assets->LinearColor()) || beforeGraph, bloom = post.bloom > 0;
        if (hdr && !hdrSupported) {
            throw std::runtime_error("GPU HDR effects require filterable, blendable RGBA16F render targets; use the software renderer on this device.");
        }
        const bool dof = post.dofRadius > 0;
        if ((dof || post.ssao > 0 || post.ssr > 0) && !dofSupported) {
            throw std::runtime_error("GPU depth of field, SSAO and SSR require renderable R32F targets; use the software renderer on this device.");
        }
        // FXAA and a post graph both read neighbours of the quantized display image (Targets::fxaa: that image exists).
        const bool displaySource = post.fxaa || (graph.graph && !beforeGraph);
        if (!t.Matches(w, h, output, hdr, bloom, displaySource, dof, dofMrtSupported && post.ssr == 0, post.ssao > 0 || post.ssr > 0, post.ssr > 0, beforeGraph))
            t.Create(w, h, single ? 1 : (hdr ? hdrSamples : settings.msaa), output, hdr, bloom, displaySource, dof, dofMrtSupported && post.ssr == 0, post.ssao > 0 || post.ssr > 0, post.ssr > 0, beforeGraph);
    }

    // ----- Resource caches --------------------------------------------------------

    // Drops GPU copies of meshes/textures the asset manager released (hot reload, scene change).
    void Evict() {
        auto sweep = [](auto& map) {
            for (auto it = map.begin(); it != map.end();) {
                if (!OwnerAlive(it->second.permanent, it->second.owner)) {
                    it->second.Destroy();
                    it = map.erase(it);
                } else {
                    ++it;
                }
            }
        };
        sweep(meshes);
        sweep(flatMeshes);
        sweep(smoothMeshes);
        sweep(textures);
        sweep(srgbTextures);
        sweep(uiTextures);
        sweep(environments);
        for (auto it = graphPrograms.begin(); it != graphPrograms.end();) {
            if (it->second.owner.expired()) {
                it->second.Destroy();
                it = graphPrograms.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = postPrograms.begin(); it != postPrograms.end();) {
            if (it->second.owner.expired() && !it->second.compiling.valid()) {
                it->second.Destroy();
                it = postPrograms.erase(it);
            } else {
                ++it;
            }
        }
    }

    // UI images and font atlas pages: one level, updated when the texels change.
    sg_view UITextureFor(const std::shared_ptr<const Texture>& tex) {
        if (!tex || tex->width <= 0 || tex->height <= 0) return whiteTex;
        auto found = uiTextures.find(tex.get());
        if (found != uiTextures.end()) {
            if (OwnerAlive(false, found->second.owner) && found->second.version == tex->version) return found->second.view;
            found->second.Destroy();
            uiTextures.erase(found);
        }
        sg_image_desc d{};
        d.width = tex->width;
        d.height = tex->height;
        d.pixel_format = kColorFormat;
        d.data.mip_levels[0] = {tex->texels.data(), tex->texels.size() * sizeof(uint32_t)};
        d.label = "oe-ui-texture";
        GpuTexture g;
        g.image = sg_make_image(&d);
        g.view = TextureView(g.image);
        g.owner = tex;
        g.version = tex->version;
        return (uiTextures[tex.get()] = g).view;
    }

    // `flat` expands every triangle to its own vertices with the face normal
    // (the software renderer's flat shading), keeping the index ranges of submeshes; `smooth` uses
    // Mesh::SmoothNormals instead of the normals (Material::smoothNormals).
    const GpuMesh& MeshFor(const std::shared_ptr<const oe::Mesh>& mesh, bool flat, bool smooth = false) {
        auto& map = smooth ? smoothMeshes : flat ? flatMeshes : meshes;
        flat = flat && !smooth;
        auto found = map.find(mesh.get());
        if (found != map.end() && OwnerAlive(found->second.permanent, found->second.owner)) return found->second;
        if (found != map.end()) {
            found->second.Destroy();
            map.erase(found);
        }
        const oe::Mesh& m = *mesh;
        std::vector<MeshVertex> verts;
        std::vector<uint32_t> indices;
        auto vertex = [&](uint32_t i, const Vec3& n) {
            MeshVertex v{};
            const Vec4 color = m.VertexColor(i, meshesLinear);
            const Vec4 data = i < m.particleData.size() ? m.particleData[i] : Vec4();
            Put(v.color, color.x, color.y, color.z, color.w);
            Put(v.particle, data.x, data.y, data.z, data.w);
            const Vec4 custom = i < m.particleCustom.size() ? m.particleCustom[i] : Vec4();
            Put(v.custom, custom.x, custom.y, custom.z, custom.w);
            const Vec4 t = static_cast<size_t>(i) < m.tangents.size() ? m.tangents[i] : Vec4(1, 0, 0, 1);
            v.tan[0] = t.x;
            v.tan[1] = t.y;
            v.tan[2] = t.z;
            v.tan[3] = t.w;
            if (i < m.skin.size()) for (size_t k = 0; k < 4; ++k) {
                // Ranges with their own palette address joints by their place in it (see ApplySkin).
                v.joints[k] = static_cast<float>(i < m.drawJoints.size() ? m.drawJoints[i][k] : m.skin[i].joints[k]);
                v.weights[k] = m.skin[i].weights[k];
            }
            const Vec3& p = m.positions[i];
            v.pos[0] = p.x;
            v.pos[1] = p.y;
            v.pos[2] = p.z;
            v.nrm[0] = n.x;
            v.nrm[1] = n.y;
            v.nrm[2] = n.z;
            if (static_cast<size_t>(i) * 2 + 1 < m.uvs.size()) {
                v.uv[0] = m.uvs[static_cast<size_t>(i) * 2];
                v.uv[1] = m.uvs[static_cast<size_t>(i) * 2 + 1];
            }
            return v;
        };
        if (flat) {
            verts.reserve(m.indices.size());
            for (size_t k = 0; k + 2 < m.indices.size(); k += 3) {
                const Vec3& a = m.positions[m.indices[k]];
                const Vec3& b = m.positions[m.indices[k + 1]];
                const Vec3& c = m.positions[m.indices[k + 2]];
                Vec3 n = Normalize(Cross(b - a, c - a));
                for (int j = 0; j < 3; ++j) {
                    verts.push_back(vertex(m.indices[k + static_cast<size_t>(j)], n));
                    indices.push_back(static_cast<uint32_t>(k) + static_cast<uint32_t>(j));
                }
            }
        } else {
            verts.reserve(m.positions.size());
            const std::vector<Vec3> smoothNormals = smooth ? m.SmoothNormals() : std::vector<Vec3>();
            const std::vector<Vec3>& normals = smooth ? smoothNormals : m.normals;
            for (size_t i = 0; i < m.positions.size(); ++i) {
                verts.push_back(vertex(static_cast<uint32_t>(i), i < normals.size() ? normals[i] : Vec3(0, 1, 0)));
            }
            indices = m.indices;
        }
        GpuMesh g;
        g.permanent = mesh.use_count() == 0;
        g.owner = mesh;
        if (verts.empty() || indices.empty()) {
            // sokol does not allow empty buffers; keep a degenerate triangle.
            verts.assign(3, MeshVertex{});
            indices = {0, 0, 0};
        }
        sg_buffer_desc vd{};
        vd.data = {verts.data(), verts.size() * sizeof(MeshVertex)};
        vd.label = "oe-mesh-vertices";
        g.vbuf = sg_make_buffer(&vd);
        sg_buffer_desc id{};
        id.usage.vertex_buffer = false;
        id.usage.index_buffer = true;
        id.data = {indices.data(), indices.size() * sizeof(uint32_t)};
        id.label = "oe-mesh-indices";
        g.ibuf = sg_make_buffer(&id);
        return map[mesh.get()] = g;
    }

    // Mipmapped GPU copy of a material texture; empty view when there is none. `srgb`: an SRGB8A8 copy, which the
    // sampler decodes to linear light before filtering (ColorTextureFor); a texture used both ways has both copies.
    sg_view TextureFor(const std::shared_ptr<const Texture>& owner, bool srgb = false) {
        const Texture* tex = owner.get();
        if (!tex || tex->width <= 0 || tex->height <= 0) return sg_view{};
        auto& map = srgb ? srgbTextures : textures;
        auto found = map.find(tex);
        if (found != map.end() && OwnerAlive(found->second.permanent, found->second.owner)) return found->second.view;
        if (found != map.end()) {
            found->second.Destroy();
            map.erase(found);
        }
        std::vector<std::vector<uint32_t>> mips = BuildMips(*tex, srgb);
        sg_image_desc d{};
        d.width = tex->width;
        d.height = tex->height;
        d.pixel_format = srgb ? SG_PIXELFORMAT_SRGB8A8 : kColorFormat;
        d.num_mipmaps = static_cast<int>(mips.size());
        for (size_t i = 0; i < mips.size(); ++i) d.data.mip_levels[i] = {mips[i].data(), mips[i].size() * sizeof(uint32_t)};
        d.label = srgb ? "oe-texture-srgb" : "oe-texture";
        GpuTexture g;
        g.image = sg_make_image(&d);
        g.view = TextureView(g.image);
        g.owner = owner;
        return (map[tex] = g).view;
    }
    // A texture in a color slot (base, emissive, decals): decoded by the sampler in a linear project
    // (Texture::Decoded), as stored otherwise.
    sg_view ColorTextureFor(const std::shared_ptr<const Texture>& owner, bool linear) {
        return TextureFor(owner, owner && owner->Decoded(linear));
    }

    // GPU copy of an environment image (its blur levels as mip levels); the white texture when there is none.
    sg_view EnvironmentFor(const std::shared_ptr<const EnvironmentMap>& owner) {
        const EnvironmentMap* map = owner.get();
        if (!map || map->levels.empty()) return whiteTex;
        auto found = environments.find(map);
        if (found != environments.end() && OwnerAlive(found->second.permanent, found->second.owner)) return found->second.view;
        if (found != environments.end()) {
            found->second.Destroy();
            environments.erase(found);
        }
        sg_image_desc d{};
        d.width = map->levels[0].width;
        d.height = map->levels[0].height;
        d.num_mipmaps = static_cast<int>(std::min<size_t>(map->levels.size(), SG_MAX_MIPMAPS));
        d.pixel_format = envHalfFloat ? kHdrFormat : kColorFormat;
        std::vector<std::vector<uint16_t>> halves;
        std::vector<std::vector<uint32_t>> bytes;
        for (int i = 0; i < d.num_mipmaps; ++i) {
            const EnvironmentMap::Level& level = map->levels[static_cast<size_t>(i)];
            const size_t texels = static_cast<size_t>(level.width) * static_cast<size_t>(level.height);
            if (envHalfFloat) {
                halves.emplace_back(texels * 4);
                for (size_t k = 0; k < texels; ++k) {
                    for (size_t c = 0; c < 3; ++c) halves.back()[k * 4 + c] = HalfFloat(level.rgb[k * 3 + c]);
                    halves.back()[k * 4 + 3] = 0x3C00;  // 1.0
                }
                d.data.mip_levels[i] = {halves.back().data(), texels * 4 * sizeof(uint16_t)};
            } else {
                bytes.emplace_back(texels);
                auto byte = [](float v) { return static_cast<uint32_t>(std::min(std::max(v, 0.0f), 1.0f) * 255.0f + 0.5f); };
                for (size_t k = 0; k < texels; ++k)
                    bytes.back()[k] = 0xFF000000u | (byte(level.rgb[k * 3 + 2]) << 16) | (byte(level.rgb[k * 3 + 1]) << 8) | byte(level.rgb[k * 3]);
                d.data.mip_levels[i] = {bytes.back().data(), texels * sizeof(uint32_t)};
            }
        }
        d.label = "oe-environment";
        GpuEnvironment g;
        g.image = sg_make_image(&d);
        g.view = TextureView(g.image);
        g.owner = owner;
        return (environments[map] = g).view;
    }

    // ----- Frame ------------------------------------------------------------------

    // The joint palette has its own uniform block, uploaded for skinned items only. Unskinned vertices
    // ignore it (zero weights), so after a pipeline switch it is filled once (zeros) and then left as is.
    bool skinApplied = false;
    void Use(sg_pipeline pipeline) {
        sg_apply_pipeline(pipeline);
        skinApplied = false;
    }
    // `submesh` selects the range's own palette in models with more joints than one draw holds.
    void ApplySkin(int slot, const RenderItem& item, int submesh) {
        static_assert(sizeof(oe_mesh_vs_skin_t) == sizeof(oe_pos_vs_skin_t), "both skin blocks hold the same palette");
        if (item.joints.empty() && skinApplied) return;
        oe_mesh_vs_skin_t skin{};
        const std::vector<uint16_t>* palette = nullptr;
        if (!item.joints.empty() && submesh >= 0 && static_cast<size_t>(submesh) < item.mesh->submeshes.size() &&
            !item.mesh->submeshes[static_cast<size_t>(submesh)].palette.empty()) palette = &item.mesh->submeshes[static_cast<size_t>(submesh)].palette;
        if (palette) {
            for (size_t j = 0; j < palette->size() && j < oe::Mesh::kDrawJoints; ++j)
                if ((*palette)[j] < item.joints.size()) Put(skin.joint_palette[j], item.joints[(*palette)[j]]);
        } else {
            for (size_t j = 0; j < item.joints.size() && j < oe::Mesh::kDrawJoints; ++j) Put(skin.joint_palette[j], item.joints[j]);
        }
        sg_apply_uniforms(slot, SG_RANGE(skin));
        skinApplied = true;
    }

    // Draws `scene` into `t` (scene + mask images) and composites into the
    // output: the offscreen output image or the window swapchain.
    // Writes this frame's batch geometry (one transient write per stream) and points the batches at it.
    void UploadBatches(DrawBatches& batching) {
        if (batching.batches.empty()) return;
        batchVertices.Write(batching.vertices.data(), batching.vertices.size() * sizeof(MeshVertex));
        batchIndices.Write(batching.indices.data(), batching.indices.size() * sizeof(uint32_t));
        for (DrawBatch& batch : batching.batches) {
            batch.mesh.vbuf = batchVertices.buf;
            batch.mesh.ibuf = batchIndices.buf;
        }
    }

    // `pipeline` is the one in use (GraphPipeline of the standard surface pipeline `standard`).
    void ApplySurface(const DrawCall& draw, const RenderItem& item, const MeshBinding& mesh, const Mat4& viewProj,
                      float time, float cutoff, sg_pipeline pipeline, sg_pipeline standard, bool depthOut = false) {
        const bool compiled = pipeline.id != standard.id;
        const Material& material = draw.material;
        oe_mesh_vs_params_t vertex{};
        Put(vertex.view_proj, viewProj);
        Put(vertex.model, item.world);
        Put(vertex.normal_mat, item.normalMatrix);
        Put(vertex.item_custom, item.particleCustom.x, item.particleCustom.y, item.particleCustom.z, item.particleCustom.w);
        Put(vertex.item_particle, item.particleInput.x, item.particleInput.y, item.particleInput.z, item.particleInput.w);
        sg_apply_uniforms(UB_oe_mesh_vs_params, SG_RANGE(vertex));
        ApplySkin(UB_oe_mesh_vs_skin, item, static_cast<int>(draw.submesh));
        oe_mesh_vs_graph_t vertexGraph{};
        PutVertexGraph(vertexGraph, material, item, time, graphViewer, compiled);
        sg_apply_uniforms(UB_oe_mesh_vs_graph, SG_RANGE(vertexGraph));
        oe_mesh_material_t uniforms{};
        const sg_view texture = ColorTextureFor(material.baseTexture, meshesLinear);
        Put(uniforms.base_color, material.baseColor.r, material.baseColor.g, material.baseColor.b, material.opacity);
        Put(uniforms.flags, 0, texture.id ? 1.0f : 0.0f, cutoff, material.doubleSided ? 1.0f : 0.0f);
        Put(uniforms.uv_rect, item.uvOffset[0], item.uvOffset[1], item.uvScale[0], item.uvScale[1]);
        Put(uniforms.uv_tiling, material.tiling[0], material.tiling[1], material.offset[0], material.offset[1]);
        Put(uniforms.emissive, 0, 0, 0, item.flat ? 1.0f : 0.0f);
        Put(uniforms.color_range, 0, depthOut ? 1.0f : 0.0f, 0, 0);
        PutGraph(uniforms, material, time, graphViewer, compiled, false);
        sg_apply_uniforms(UB_oe_mesh_material, SG_RANGE(uniforms));
        sg_bindings bindings{};
        Bind(bindings, mesh);
        bindings.views[VIEW_oe_base_tex] = texture.id ? texture : whiteTex;
        bindings.samplers[SMP_oe_base_smp] = material.pixelArt ? pixelArt : linearRepeat;
        sg_apply_bindings(&bindings);
    }

    // Immutable replacements also permit several editor viewports with different scenes in one sokol frame.
    sg_view SpotTextureFor(const RenderLights& lights, const std::vector<RenderProbe>& probes, const LocalShadowSet& shadows,
                          const RenderPlanarReflection& planar, const std::vector<RenderDecal>& decals, const RenderView& view, int width, int height) {
        const PostProcess post = NormalizePostProcess(view.postProcess);
        if (lights.spots.empty() && !lights.env.map && probes.empty() && shadows.lights.empty() && planar.id == kNullEntity && decals.empty() && post.ssao == 0 && post.ssr == 0) return whiteTex;
        float data[kLocalLightDataWidth * kLocalLightDataHeight * 4]{};
        if (planar.id != kNullEntity) Put(data + kPlanarMatrixTexel * 4, planar.view.proj * planar.view.view);
        Put(data + 148*4,post.ssaoRadius,post.ssaoBias,static_cast<float>(width),static_cast<float>(height));
        Put(data + 149*4,static_cast<float>(decals.size()),0,0,0);
        for (size_t i = 0; i < decals.size(); ++i) {
            const RenderDecal& decal = decals[i];
            const size_t start = static_cast<size_t>(kDecalDataStart) + i*kDecalDataStride;
            Put(data + start*4,decal.worldToBox);
            Put(data + (start+4)*4,decal.color.r,decal.color.g,decal.color.b,decal.opacity);
            Put(data + (start+5)*4,decal.edgeFade,decal.normalThreshold,0,0);
            Put(data + (start+6)*4,decal.normal.x,decal.normal.y,decal.normal.z,0);
        }
        for (size_t i = 0; i < lights.spots.size(); ++i) {
            const RenderSpotLight& light = lights.spots[i];
            Put(data + i * 4, light.pos.x, light.pos.y, light.pos.z, light.range);
            Put(data + (kMaxSpotLights + i) * 4, light.dir.x, light.dir.y, light.dir.z, light.outerCos);
            Put(data + (kMaxSpotLights * 2 + i) * 4, light.color.r, light.color.g, light.color.b, light.innerCos);
        }
        for (int i = 0; i < 9; ++i) {
            const Vec3& term = lights.env.sh[i];
            Put(data + (24 + i) * 4, term.x, term.y, term.z, 0);
        }
        for (size_t i = 0; i < probes.size(); ++i) for (int k = 0; k < 9; ++k) {
            const Vec3& term = probes[i].sh[k];
            Put(data + (33 + i * 9 + k) * 4, term.x, term.y, term.z, 0);
        }
        for (size_t i = 0; i < shadows.lights.size(); ++i) {
            const auto& light = shadows.lights[i];
            Put(data + (kLocalShadowConfigTexel + i) * 4, static_cast<float>(light.firstMap),
                static_cast<float>(light.mapCount), 0, 0);
        }
        for (size_t i = 0; i < lights.spots.size(); ++i)
            data[(kSpotShadowSlotTexel + i) * 4] = static_cast<float>(LocalShadowSlot(shadows, false, i) + 1);
        for (size_t i = 0; i < shadows.maps.size(); ++i) {
            const auto& map = shadows.maps[i];
            Put(data + (kLocalShadowMatrixTexel + i * 4) * 4, map.viewProj);
            Put(data + (kLocalShadowParamsTexel + i) * 4, map.texelScale, map.nearPlane, map.farPlane, 0);
        }
        if (spotTex.id && std::memcmp(data, spotData, sizeof(data)) == 0) return spotTex;
        if (spotTex.id) sg_destroy_view(spotTex);
        if (spotImage.id) sg_destroy_image(spotImage);
        sg_image_desc desc{};
        desc.width = kLocalLightDataWidth; desc.height = kLocalLightDataHeight;
        desc.pixel_format = SG_PIXELFORMAT_RGBA32F;
        desc.data.mip_levels[0] = {data, sizeof(data)};
        desc.label = "oe-spot-lights";
        spotImage = sg_make_image(&desc);
        spotTex = TextureView(spotImage);
        std::memcpy(spotData, data, sizeof(data));
        return spotTex;
    }

    RenderStats Frame(const Scene& scene, const RenderView& view, Targets& t, int outW, int outH, const sg_swapchain* swapchain) {
        auto start = std::chrono::steady_clock::now();
        graphViewer = ShaderViewer(view);
        RenderStats stats;
        Evict();
        std::vector<RenderItem> items = GatherRenderItems(scene, assets, view.view, view.shaderTime, view.pixelStep);
        items.erase(std::remove_if(items.begin(), items.end(), [&](const RenderItem& it) { return it.id == view.excludeEntity; }), items.end());
        const RenderPlanarReflection planar = SelectPlanarReflection(scene, items, view);
        if (planar.id != kNullEntity) {
            PrepareTargets(planarTarget, planar.resolution, planar.resolution, true, planar.view);
            Frame(scene, planar.view, planarTarget, planar.resolution, planar.resolution, nullptr);
        } else if (!view.reflectionCapture) {
            planarTarget.Destroy();
        }
        const std::vector<RenderDecal> decals = GatherDecals(scene,assets,view);
        const std::shared_ptr<const Texture> decalPicture = decalAtlas.Prepare(decals);
        RenderLights lights = GatherRenderLights(scene, assets);
        if (lights.linear != meshesLinear) {  // the color space changed (another project): rebuild the vertex colors
            for (auto* map : {&meshes, &flatMeshes, &smoothMeshes}) {
                for (auto& kv : *map) kv.second.Destroy();
                map->clear();
            }
            meshesLinear = lights.linear;
        }
        const LocalShadowSet localShadows = SelectLocalShadows(lights, view.proj * view.view, view.eye);
        if (!localShadows.maps.empty() && !localShadowAtlas.id) {
            localShadowAtlas = MakeAttachmentImage(kLocalShadowAtlasSize, kLocalShadowAtlasSize, kDepthFormat, 1,
                                                 true, false, "oe-local-shadow-atlas");
            localShadowAtt = DepthView(localShadowAtlas);
            localShadowTex = TextureView(localShadowAtlas);
        }
        if (view.editorFill > 0.0f) {  // editor fill: lift the ambient floor so a script-lit scene is visible to edit (matches the software renderer)
            lights.ambient = Color(std::max(lights.ambient.r, view.editorFill), std::max(lights.ambient.g, view.editorFill),
                                   std::max(lights.ambient.b, view.editorFill));
            lights.groundAmbient = Color(std::max(lights.groundAmbient.r, view.editorFill), std::max(lights.groundAmbient.g, view.editorFill),
                                         std::max(lights.groundAmbient.b, view.editorFill));
        }
        const GraphViewInputs graphView = MakeGraphViewInputs(view.proj, t.width, t.height);
        const std::vector<DrawCall> draws = BuildDrawList(items, view.eye, &lights, view.playing, &graphView);
        // Meshes that are gone (blend shape weights changed, assets reloaded) give their buffers back.
        for (auto* map : {&meshes, &flatMeshes, &smoothMeshes}) {
            for (auto it = map->begin(); it != map->end();) {
                if (OwnerAlive(it->second.permanent, it->second.owner)) { ++it; continue; }
                it->second.Destroy();
                it = map->erase(it);
            }
        }
        std::vector<const GpuMesh*> gpuMeshes;
        gpuMeshes.reserve(items.size());
        for (const RenderItem& it : items) {
            gpuMeshes.push_back(&MeshFor(it.mesh, it.flat));
            stats.triangles += static_cast<int>(it.mesh->TriangleCount());
        }
        // Draws whose material asks for smoothed normals use that copy of the mesh.
        std::vector<const GpuMesh*> drawMeshes(draws.size());
        for (size_t k = 0; k < draws.size(); ++k)
            drawMeshes[k] = draws[k].material.smoothNormals ? &MeshFor(items[draws[k].item].mesh, false, true) : gpuMeshes[draws[k].item];
        stats.drawnEntities = static_cast<int>(items.size());
        // Frustum culling: each draw's world box (displaced surfaces widened by their offset margin)
        // against the camera here and against the light in the shadow pass.
        std::vector<Vec3> drawLo(draws.size()), drawHi(draws.size());
        std::vector<char> inView(draws.size());
        const Mat4 cameraViewProj = view.proj * view.view;
        for (size_t k = 0; k < draws.size(); ++k) {
            Vec3 offsetLo, offsetHi;
            OffsetRange(draws[k].material, offsetLo, offsetHi);
            ItemWorldBounds(items[draws[k].item], offsetLo, offsetHi, drawLo[k], drawHi[k]);
            inView[k] = BoxInFrustum(cameraViewProj, drawLo[k], drawHi[k]);
        }

        // ----- Shadow map: first directional light, fitted to casters + receivers
        bool shadows = false;
        ShadowFit fit;
        ShadowCascadeSet cascades;
        if (lights.shadows && !items.empty()) {
            Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
            bool anyCaster = false;
            for (const RenderItem& it : items) {
                if (it.unlit) continue;
                anyCaster = anyCaster || it.castShadows;
                const Vec3& a = it.boundsMin;
                const Vec3& b = it.boundsMax;
                for (int c = 0; c < 8; ++c) {
                    Vec3 p = it.world.TransformPoint(Vec3(c & 1 ? b.x : a.x, c & 2 ? b.y : a.y, c & 4 ? b.z : a.z));
                    lo = Vec3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
                    hi = Vec3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
                }
            }
            ExtendOffsetBounds(items, draws, lo, hi);
            if (anyCaster && lo.x <= hi.x) {
                shadows = true;
                cascades = FitShadowCascades(view.view, view.proj, lights.shadowDistance, lights.shadowCascades,
                                            lo, hi, lights.dirs[0].dir, settings.shadowMapSize);
                fit = cascades.fits[0];
                if (cascades.count > 1 && !cascadeAtlas.id) {
                    cascadeAtlas = MakeAttachmentImage(kCascadeMapSize * 2, kCascadeMapSize * 2, kDepthFormat, 1,
                                                       true, false, "oe-cascade-atlas");
                    cascadeAtt = DepthView(cascadeAtlas);
                    cascadeTex = TextureView(cascadeAtlas);
                }
            }
        }

        // ----- What each pass draws: culled draws in draw-list order, runs of small meshes batched
        std::vector<size_t> shadowOrder, localShadowOrder, sceneOrder;
        for (size_t k = 0; k < draws.size(); ++k) {
            const RenderItem& it = items[draws[k].item];
            // Transparent surfaces cast no shadow.
            bool inShadow = false;
            if (shadows) for (int cascade = 0; cascade < cascades.count; ++cascade)
                inShadow = inShadow || BoxInFrustum(cascades.fits[static_cast<size_t>(cascade)].viewProj, drawLo[k], drawHi[k], false);
            // Shadows come from opaque surfaces whose material casts them; extra passes (nextPass, outline
            // shells) only add color.
            const bool caster = it.castShadows && !it.unlit && !draws[k].blend && draws[k].pass == 0 && draws[k].material.castShadows;
            if (shadows && caster && inShadow)
                shadowOrder.push_back(k);
            if (caster) {
                bool inLocalShadow = false;
                for (const auto& map : localShadows.maps)
                    inLocalShadow = inLocalShadow || BoxInFrustum(map.viewProj, drawLo[k], drawHi[k]);
                if (inLocalShadow) localShadowOrder.push_back(k);
            }
            if (inView[k]) sceneOrder.push_back(k);
        }
        DrawBatches batching;
        const std::vector<DrawPiece> shadowPieces = PlanBatches(items, draws, shadowOrder, batching, kNullEntity, lights.linear);
        const std::vector<DrawPiece> localShadowPieces = PlanBatches(items, draws, localShadowOrder, batching, kNullEntity, lights.linear);
        const std::vector<DrawPiece> scenePieces = PlanBatches(items, draws, sceneOrder, batching, planar.id, lights.linear);
        UploadBatches(batching);
        // The item, draw call and geometry of a piece: the draw's own, or its batch's.
        auto piece = [&](const DrawPiece& p, const DrawCall*& dc, const RenderItem*& it, MeshBinding& mesh) {
            if (p.batch >= 0) {
                const DrawBatch& batch = batching.batches[static_cast<size_t>(p.batch)];
                dc = &batch.call;
                it = &batch.item;
                mesh = batch.mesh;
            } else {
                dc = &draws[p.draw];
                it = &items[dc->item];
                mesh = Binding(*drawMeshes[p.draw], it->mesh->submeshes[dc->submesh]);
            }
        };

        auto drawShadows = [&](const std::vector<DrawPiece>& pieces, const Mat4& shadowViewProj) {
            int current = -1;
            for (const DrawPiece& p : pieces) {
                const DrawCall* dcp;
                const RenderItem* itp;
                MeshBinding mesh;
                piece(p, dcp, itp, mesh);
                const DrawCall& dc = *dcp;
                const RenderItem& it = *itp;
                const bool cutout = dc.material.alphaMode == AlphaMode::Mask && (dc.material.shader || dc.material.baseTexture);
                if (cutout || Displaced(dc.material)) {
                    const sg_pipeline standard = surfaceShadow[dc.material.doubleSided ? 1 : 0], pipeline = GraphPipeline(standard, dc.material);
                    Use(pipeline);
                    ApplySurface(dc, it, mesh, shadowViewProj, view.shaderTime, cutout ? dc.material.alphaCutoff : 0.0f, pipeline, standard);
                    Draw(mesh);
                    current = -1;
                    continue;
                }
                int want = dc.material.doubleSided ? 1 : 0;
                if (want != current) {
                    Use(want ? shadowPipTwoSided : shadowPip);
                    current = want;
                }
                oe_pos_vs_params_t u{};
                Put(u.mvp, shadowViewProj * it.world);
                sg_apply_uniforms(UB_oe_pos_vs_params, SG_RANGE(u));
                ApplySkin(UB_oe_pos_vs_skin, it, static_cast<int>(dc.submesh));
                sg_bindings b{};
                Bind(b, mesh);
                sg_apply_bindings(&b);
                Draw(mesh);
            }
        };

        if (shadows) {
            sg_pass pass{};
            pass.action.depth.load_action = SG_LOADACTION_CLEAR;
            pass.action.depth.clear_value = 1.0f;
            pass.action.depth.store_action = SG_STOREACTION_STORE;
            pass.attachments.depth_stencil = cascades.count > 1 ? cascadeAtt : shadowAtt;
            pass.label = "oe-shadow";
            sg_begin_pass(&pass);
            for (int cascade = 0; cascade < cascades.count; ++cascade) {
                const ShadowFit& cascadeFit = cascades.fits[static_cast<size_t>(cascade)];
                if (cascades.count > 1) {
                    sg_apply_viewport((cascade % 2) * kCascadeMapSize, (cascade / 2) * kCascadeMapSize,
                                      kCascadeMapSize, kCascadeMapSize, true);
                    sg_apply_scissor_rect((cascade % 2) * kCascadeMapSize, (cascade / 2) * kCascadeMapSize,
                                          kCascadeMapSize, kCascadeMapSize, true);
                }
                drawShadows(shadowPieces, cascadeFit.viewProj);
            }
            sg_end_pass();
        }

        if (!localShadows.maps.empty()) {
            sg_pass pass{};
            pass.action.depth.load_action = SG_LOADACTION_CLEAR;
            pass.action.depth.clear_value = 1;
            pass.action.depth.store_action = SG_STOREACTION_STORE;
            pass.attachments.depth_stencil = localShadowAtt;
            pass.label = "oe-local-shadows";
            sg_begin_pass(&pass);
            for (size_t i = 0; i < localShadows.maps.size(); ++i) {
                const int tile = static_cast<int>(i);
                sg_apply_viewport((tile % 4) * kLocalShadowTileSize, (tile / 4) * kLocalShadowTileSize,
                                  kLocalShadowTileSize, kLocalShadowTileSize, true);
                sg_apply_scissor_rect((tile % 4) * kLocalShadowTileSize, (tile / 4) * kLocalShadowTileSize,
                                      kLocalShadowTileSize, kLocalShadowTileSize, true);
                drawShadows(localShadowPieces, localShadows.maps[i].viewProj);
            }
            sg_end_pass();
        }

        const Mat4 viewProj = view.proj * view.view;

        // ----- Lines: editor grid, colliders, debug.draw
        std::vector<ColorVertex> lineVerts;
        auto line = [&](const Vec3& a, const Vec3& b, const Color& c, float alpha) {
            // Clip to the view frustum on the CPU (Liang-Barsky in clip space):
            // lines reaching far off-screen or behind the camera are not
            // rasterized reliably by every driver.
            Vec4 ca = viewProj * Vec4(a, 1.0f), cb = viewProj * Vec4(b, 1.0f);
            float t0 = 0.0f, t1 = 1.0f;
            const float pa[6] = {ca.w + ca.x, ca.w - ca.x, ca.w + ca.y, ca.w - ca.y, ca.w + ca.z, ca.w - ca.z};
            const float pb[6] = {cb.w + cb.x, cb.w - cb.x, cb.w + cb.y, cb.w - cb.y, cb.w + cb.z, cb.w - cb.z};
            for (int i = 0; i < 6; ++i) {
                if (pa[i] < 0 && pb[i] < 0) return;
                if (pa[i] < 0) t0 = std::max(t0, pa[i] / (pa[i] - pb[i]));
                else if (pb[i] < 0) t1 = std::min(t1, pa[i] / (pa[i] - pb[i]));
            }
            if (t0 >= t1) return;
            Vec3 p = a + (b - a) * t0, q = a + (b - a) * t1;
            const Color d = SceneColor(c, lights.linear);
            lineVerts.push_back({{p.x, p.y, p.z}, {d.r, d.g, d.b, alpha}});
            lineVerts.push_back({{q.x, q.y, q.z}, {d.r, d.g, d.b, alpha}});
        };
        if (view.drawGrid) {
            const Color gridColor(0.55f, 0.58f, 0.62f);
            for (int i = -20; i <= 20; ++i) {
                float f = static_cast<float>(i);
                if (i == 0) continue;
                line(Vec3(f, 0, -20), Vec3(f, 0, 20), gridColor, 0.25f);
                line(Vec3(-20, 0, f), Vec3(20, 0, f), gridColor, 0.25f);
            }
            line(Vec3(-20, 0, 0), Vec3(20, 0, 0), Color(0.9f, 0.25f, 0.25f), 0.8f);  // X axis
            line(Vec3(0, 0, -20), Vec3(0, 0, 20), Color(0.25f, 0.45f, 0.95f), 0.8f);  // Z axis
        }
        for (const DebugLine& l : view.lines) line(l.a, l.b, l.color, 1.0f);
        bool haveLines = lines.Write(lineVerts.data(), lineVerts.size() * sizeof(ColorVertex));

        // Shared opaque/cutout depth prepass. SSAO needs it before ambient lighting.
        auto drawDepth = [&](sg_view attachment) {
            sg_pass pass{};
            pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
            pass.action.colors[0].clear_value = {1, 0, 0, 1};
            pass.action.depth.load_action = SG_LOADACTION_CLEAR;
            pass.action.depth.clear_value = 1.0f;
            pass.attachments.colors[0] = attachment;
            pass.attachments.depth_stencil = t.maskDepthAtt;
            pass.label = "oe-scene-depth-prepass";
            sg_begin_pass(&pass);
            for (const DrawCall& dc : draws) {
                if (dc.blend || dc.pass > 0 || !inView[static_cast<size_t>(&dc - draws.data())]) continue;  // the surface only
                const RenderItem& it = items[dc.item];
                const Submesh& sub = it.mesh->submeshes[dc.submesh];
                const int twoSided = dc.material.doubleSided ? 1 : 0;
                const bool cutout = dc.material.alphaMode == AlphaMode::Mask;
                if (cutout || Displaced(dc.material)) {
                    const sg_pipeline pipeline = GraphPipeline(surfaceDepthPip[twoSided], dc.material);
                    Use(pipeline);
                    ApplySurface(dc, it, Binding(*drawMeshes[static_cast<size_t>(&dc - draws.data())], sub), viewProj, view.shaderTime,
                                 cutout ? dc.material.alphaCutoff : 0.0f, pipeline, surfaceDepthPip[twoSided], true);
                } else {
                    Use(sceneDepthPip[twoSided]);
                    oe_pos_vs_params_t u{};
                    Put(u.mvp, viewProj * it.world);
                    sg_apply_uniforms(UB_oe_pos_vs_params, SG_RANGE(u));
                    ApplySkin(UB_oe_pos_vs_skin, it, static_cast<int>(dc.submesh));
                    sg_bindings b{};
                    b.vertex_buffers[0] = gpuMeshes[dc.item]->vbuf;
                    b.index_buffer = gpuMeshes[dc.item]->ibuf;
                    sg_apply_bindings(&b);
                }
                sg_draw(static_cast<int>(sub.firstIndex), static_cast<int>(sub.indexCount), 1);
            }
            sg_end_pass();
        };
        if (t.ssao) drawDepth(t.ssaoDepthAtt);

        // ----- Scene pass (MSAA, resolved into t.sceneColor)
        {
            sg_pass pass{};
            pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
            const Color clear = t.hdr ? ClampHdr(SceneColor(view.clearColor, lights.linear)) : view.clearColor;
            pass.action.colors[0].clear_value = {clear.r, clear.g, clear.b, 1.0f};
            pass.action.colors[0].store_action = t.msaa > 1 ? SG_STOREACTION_DONTCARE : SG_STOREACTION_STORE;
            pass.action.depth.load_action = SG_LOADACTION_CLEAR;
            pass.action.depth.clear_value = 1.0f;
            if (t.msaa > 1) {
                pass.attachments.colors[0] = t.sceneMsaaAtt;
                pass.attachments.resolves[0] = t.sceneColorAtt;
            } else {
                pass.attachments.colors[0] = t.sceneColorAtt;
            }
            if (t.dofMrt) {
                // Background: the far plane, as the depth buffer's clear value gives the separate depth pass.
                const float p22 = view.proj.at(2, 2), p23 = view.proj.at(2, 3);
                const float farDepth = view.proj.at(3, 3) == 1.0f ? (p23 - 1.0f) / p22 : p23 / (1.0f + p22);
                pass.action.colors[1].load_action = SG_LOADACTION_CLEAR;
                pass.action.colors[1].clear_value = {farDepth, 0.0f, 0.0f, 1.0f};
                pass.action.colors[1].store_action = t.msaa > 1 ? SG_STOREACTION_DONTCARE : SG_STOREACTION_STORE;
                pass.attachments.colors[1] = t.msaa > 1 ? t.dofDepthMsaaAtt : t.dofDepthAtt;
                if (t.msaa > 1) pass.attachments.resolves[1] = t.dofDepthResolveAtt;
            }
            if (t.ssr) for (int i = 0; i < 3; ++i) {
                pass.action.colors[i + 1].load_action = SG_LOADACTION_CLEAR;
                pass.action.colors[i + 1].clear_value = {0, 0, 0, i == 0 ? 1.0f : 0.0f};
                pass.action.colors[i + 1].store_action = t.msaa > 1 ? SG_STOREACTION_DONTCARE : SG_STOREACTION_STORE;
                pass.attachments.colors[i + 1] = t.msaa > 1 ? t.ssrMsaaAtt[i] : t.ssrAtt[i];
                if (t.msaa > 1) pass.attachments.resolves[i + 1] = t.ssrAtt[i];
            }
            pass.attachments.depth_stencil = t.sceneDepthAtt;
            pass.label = "oe-scene";
            sg_begin_pass(&pass);

            RenderEnvironment env = lights.env;  // analytic sky environment (image-based lighting + background)
            if (view.editorFill > 0.0f && env.present) {  // editor fill lifts a sky environment's floor (matches the software renderer)
                auto lift = [&](Color& c) { c = Color(std::max(c.r, view.editorFill), std::max(c.g, view.editorFill), std::max(c.b, view.editorFill)); };
                lift(env.sky); lift(env.horizon); lift(env.ground);
                if (env.map) env.sh[0] = env.sh[0] + Vec3(view.editorFill, view.editorFill, view.editorFill);
            }
            const sg_view envTex = EnvironmentFor(env.map);  // the white texture without an image
            if (env.present && env.background) {
                // Skybox background: fill the frame with the sky before any geometry (depth test always/no write).
                sg_apply_pipeline(skyPips[t.msaa > 1 ? 0 : 1][t.hdr ? 1 : 0][t.ssr ? 2 : (t.dofMrt ? 1 : 0)]);
                oe_sky_params_t sp{};
                Put(sp.inv_view_proj, (SkyRayProjection(view) * view.view).Inverse());
                Put(sp.sky_c, env.sky.r, env.sky.g, env.sky.b, 0);
                Put(sp.horizon_c, env.horizon.r, env.horizon.g, env.horizon.b, 0);
                Put(sp.ground_c, env.ground.r, env.ground.g, env.ground.b, 0);
                const Vec3 toSun = env.sunDir * -1.0f;
                Put(sp.sun_p, toSun.x, toSun.y, toSun.z, env.sunCos);
                Put(sp.sun_c, env.sunColor.r, env.sunColor.g, env.sunColor.b, env.sun ? 1.0f : 0.0f);
                Put(sp.sky_map, env.map ? 1.0f : 0.0f, env.intensity, env.yawCos, env.yawSin);
                sg_apply_uniforms(UB_oe_sky_params, SG_RANGE(sp));
                sg_bindings skyBind{};
                skyBind.views[VIEW_oe_sky_tex] = envTex;
                skyBind.samplers[SMP_oe_sky_smp] = envSampler;
                sg_apply_bindings(&skyBind);
                sg_draw(0, 3, 1);
            }

            oe_mesh_lights_t lu{};
            Put(lu.shadow_vp, fit.viewProj);
            Put(lu.ambient, lights.ambient.r, lights.ambient.g, lights.ambient.b, 0);
            int dirs = std::min(kMaxDirLights, static_cast<int>(lights.dirs.size()));
            int points = std::min(kMaxPointLights, static_cast<int>(lights.points.size()));
            Put(lu.counts, static_cast<float>(dirs), static_cast<float>(points), shadows ? 1.0f : 0.0f, lights.shadowStrength);
            Put(lu.shadow_params, fit.texelWorld,
                1.0f / static_cast<float>(cascades.count > 1 ? kCascadeMapSize : settings.shadowMapSize),
                0.002f, static_cast<float>(cascades.count));
            for (int i = 0; i < kMaxShadowCascades; ++i) {
                if (i > 0) Put(lu.cascade_vp[i - 1], cascades.fits[static_cast<size_t>(i)].viewProj);
                lu.cascade_splits[i] = cascades.splits[static_cast<size_t>(i)];
                lu.cascade_blend[i] = cascades.blendStarts[static_cast<size_t>(i)];
                lu.cascade_texels[i] = cascades.fits[static_cast<size_t>(i)].texelWorld;
                lu.cascade_bias[i] = cascades.fits[static_cast<size_t>(i)].depthBias;
            }
            Put(lu.eye, view.eye.x, view.eye.y, view.eye.z, 1);
            const PostProcess fog = NormalizePostProcess(view.postProcess);
            Put(lu.ambient_ground, lights.groundAmbient.r, lights.groundAmbient.g, lights.groundAmbient.b, lights.hemisphere ? 1.0f : 0.0f);
            const Color fogColor = SceneColor(fog.fogColor, lights.linear);
            Put(lu.fog_color, fogColor.r, fogColor.g, fogColor.b, fog.fog);
            Put(lu.fog_range, fog.fogStart, fog.fogEnd, static_cast<float>(lights.spots.size()), static_cast<float>(decals.size()));
            // Camera forward: minus the third row of the view matrix.
            Put(lu.view_axis, -view.view.at(2, 0), -view.view.at(2, 1), -view.view.at(2, 2), 0);
            for (int i = 0; i < dirs; ++i) {
                const RenderDirLight& l = lights.dirs[static_cast<size_t>(i)];
                Put(lu.dir_dir[i], l.dir.x, l.dir.y, l.dir.z, 0);
                Put(lu.dir_color[i], l.color.r, l.color.g, l.color.b, 0);
            }
            for (int i = 0; i < points; ++i) {
                const RenderPointLight& l = lights.points[static_cast<size_t>(i)];
                Put(lu.point_pos[i], l.pos.x, l.pos.y, l.pos.z, l.range);
                Put(lu.point_color[i], l.color.r, l.color.g, l.color.b,
                    static_cast<float>(LocalShadowSlot(localShadows, true, static_cast<size_t>(i)) + 1));
            }
            Put(lu.env_sky, env.sky.r, env.sky.g, env.sky.b, env.present ? 1.0f : 0.0f);
            Put(lu.env_horizon, env.horizon.r, env.horizon.g, env.horizon.b, env.reflections);
            Put(lu.env_ground, env.ground.r, env.ground.g, env.ground.b, env.intensity);
            Put(lu.env_map, env.map ? 1.0f : 0.0f, env.map ? env.map->MaxLod() : 0.0f, env.yawCos, env.yawSin);
            // Reflection probes of this frame; the slots without one keep the white texture.
            const std::vector<RenderProbe> probes = ActiveProbes(scene, assets, lights, viewProj, view.eye);
            const sg_view spotLightTex = SpotTextureFor(lights, probes, localShadows, planar, decals, view, t.width, t.height);
            const sg_view decalTexture = ColorTextureFor(decalPicture, meshesLinear);
            sg_view probeTex[kMaxProbes];
            Put(lu.probe_count, static_cast<float>(probes.size()), 0, 0, 0);
            const PostProcess ambientPost = NormalizePostProcess(view.postProcess);
            Put(lu.ssao_inverse, viewProj.Inverse());
            Put(lu.ssao_proj, view.proj.at(0, 0), view.proj.at(1, 1), view.proj.at(3, 3) == 1 ? 1.0f : 0.0f, ambientPost.ssao);
            for (int i = 0; i < kMaxProbes; ++i) {
                probeTex[i] = whiteTex;
                if (static_cast<size_t>(i) >= probes.size()) continue;
                const RenderProbe& p = probes[static_cast<size_t>(i)];
                probeTex[i] = EnvironmentFor(p.map);
                Put(lu.probe_lo[i], p.lo.x, p.lo.y, p.lo.z, p.blend);
                Put(lu.probe_hi[i], p.hi.x, p.hi.y, p.hi.z, p.map->MaxLod());
                Put(lu.probe_pos[i], p.pos.x, p.pos.y, p.pos.z, p.intensity);
                Put(lu.probe_opt[i], p.boxProjection ? 1.0f : 0.0f, p.ambient ? 1.0f : 0.0f, 0, 0);
            }
            // Draw list order: opaque, then transparent back to front. Switching
            // pipelines drops uniforms, so the lights are re-applied each time.
            sg_pipeline current{};
            for (const DrawPiece& p : scenePieces) {
                const DrawCall* dcp;
                const RenderItem* itp;
                MeshBinding mesh;
                piece(p, dcp, itp, mesh);
                const DrawCall& dc = *dcp;
                const RenderItem& it = *itp;
                const Material& m = dc.material;
                const int blendMode = dc.blend ? (m.additive ? 2 : 1) : 0;
                const int mrt = t.ssr ? 2 : (t.dofMrt ? 1 : 0), single = t.msaa > 1 ? 0 : 1;
                const sg_pipeline standardPip = t.hdr ? hdrMeshPips[single][mrt][blendMode][FaceMode(m)] : meshPips[single][mrt][blendMode][FaceMode(m)];
                sg_pipeline pip = GraphPipeline(standardPip, m);
                if (pip.id != current.id) {
                    Use(pip);
                    sg_apply_uniforms(UB_oe_mesh_lights, SG_RANGE(lu));
                    current = pip;
                }
                oe_mesh_vs_params_t vu{};
                Put(vu.view_proj, viewProj);
                Put(vu.model, it.world);
                Put(vu.normal_mat, it.normalMatrix);
                Put(vu.item_custom, it.particleCustom.x, it.particleCustom.y, it.particleCustom.z, it.particleCustom.w);
                Put(vu.item_particle, it.particleInput.x, it.particleInput.y, it.particleInput.z, it.particleInput.w);
                sg_view base = ColorTextureFor(m.baseTexture, meshesLinear);
                sg_view normal = m.unlit && !m.light2DOn ? sg_view{} : TextureFor(m.normalTexture);
                // A sprite mask takes the metallic-roughness slot (sprites have no such map; mesh_fs mask2d_alpha).
                sg_view mr = m.mask2DOn ? TextureFor(m.mask2DTexture) : m.unlit ? sg_view{} : TextureFor(m.metallicRoughnessTexture);
                sg_view emissive = ColorTextureFor(m.emissiveTexture, meshesLinear);
                // light2D surfaces are unlit: their light map takes the occlusion slot (mesh_fs light2d_color).
                sg_view occlusion = m.light2DOn ? (m.light2D ? TextureFor(m.light2D->texture) : sg_view{}) : m.unlit ? sg_view{} : TextureFor(m.occlusionTexture);
                PutSprite2D(vu, m, occlusion.id != 0, mr.id != 0);
                sg_apply_uniforms(UB_oe_mesh_vs_params, SG_RANGE(vu));
                ApplySkin(UB_oe_mesh_vs_skin, it, static_cast<int>(dc.submesh));
                oe_mesh_vs_graph_t vg{};
                PutVertexGraph(vg, m, it, view.shaderTime, graphViewer, pip.id != standardPip.id);
                sg_apply_uniforms(UB_oe_mesh_vs_graph, SG_RANGE(vg));
                Color em = m.emissive * m.emissiveIntensity;
                oe_mesh_material_t mu{};
                Put(mu.base_color, m.baseColor.r, m.baseColor.g, m.baseColor.b, m.opacity);
                Put(mu.flags, m.unlit ? 1.0f : 0.0f, base.id ? 1.0f : 0.0f, m.alphaMode == AlphaMode::Mask ? m.alphaCutoff : 0.0f,
                    m.doubleSided ? 1.0f : 0.0f);
                Put(mu.uv_rect, it.uvOffset[0], it.uvOffset[1], it.uvScale[0], it.uvScale[1]);
                Put(mu.uv_tiling, m.tiling[0], m.tiling[1], m.offset[0], m.offset[1]);
                Put(mu.pbr, m.metallic, m.roughness, m.normalScale, m.occlusionStrength);
                Put(mu.emissive, em.r, em.g, em.b, it.flat ? 1.0f : 0.0f);
                Put(mu.maps, normal.id ? 1.0f : 0.0f, mr.id ? 1.0f : 0.0f, emissive.id ? 1.0f : 0.0f, occlusion.id ? 1.0f : 0.0f);
                const float fogMode = !m.fog ? 0.0f : (dc.blend && m.additive ? 2.0f : 1.0f);
                Put(mu.color_range, t.hdr ? 65504.0f : 1.0f, it.id == planar.id ? planar.strength : 0, fogMode, (dc.blend ? 1.0f : 0.0f) + (t.ssr ? 2.0f : 0.0f));
                if (m.light2DOn) Put(mu.maps, normal.id ? 1.0f : 0.0f, 0, emissive.id ? 1.0f : 0.0f, 0);
                if (m.mask2DOn) mu.maps[1] = 0;
                PutGraph(mu, m, view.shaderTime, graphViewer, pip.id != standardPip.id, true);
                sg_apply_uniforms(UB_oe_mesh_material, SG_RANGE(mu));
                sg_bindings b{};
                Bind(b, mesh);
                b.views[VIEW_oe_base_tex] = base.id ? base : whiteTex;
                b.views[VIEW_oe_shadow_tex] = cascades.count > 1 ? cascadeTex : shadowTex;
                b.views[VIEW_oe_normal_tex] = normal.id ? normal : whiteTex;
                b.views[VIEW_oe_mr_tex] = mr.id ? mr : whiteTex;
                b.views[VIEW_oe_emissive_tex] = emissive.id ? emissive : whiteTex;
                b.views[VIEW_oe_occlusion_tex] = occlusion.id ? occlusion : whiteTex;
                b.views[VIEW_oe_env_tex] = envTex;
                b.views[VIEW_oe_probe0_tex] = probeTex[0];
                b.views[VIEW_oe_probe1_tex] = probeTex[1];
                b.views[VIEW_oe_probe2_tex] = probeTex[2];
                b.views[VIEW_oe_probe3_tex] = probeTex[3];
                b.views[VIEW_oe_spot_tex] = spotLightTex;
                b.views[VIEW_oe_local_shadow_tex] = localShadowTex.id ? localShadowTex : shadowTex;
                b.views[VIEW_oe_ssao_depth_tex] = t.ssao ? t.ssaoDepthTex : whiteTex;
                b.views[VIEW_oe_planar_tex] = planar.id != kNullEntity ? planarTarget.sceneTex : whiteTex;
                b.views[VIEW_oe_decal_tex] = decalTexture.id ? decalTexture : whiteTex;
                b.samplers[SMP_oe_planar_smp] = linearClamp;
                b.samplers[SMP_oe_spot_smp] = nearestClamp;
                b.samplers[SMP_oe_env_smp] = envSampler;
                b.samplers[SMP_oe_base_smp] = m.pixelArt ? pixelArt : linearRepeat;
                b.samplers[SMP_oe_shadow_smp] = shadowCompare;
                sg_apply_bindings(&b);
                Draw(mesh);
                ++stats.drawCalls;
            }

            if (haveLines) {
                Use(t.hdr ? hdrLinePips[t.msaa > 1 ? 0 : 1][t.ssr ? 2 : (t.dofMrt ? 1 : 0)] : linePips[t.msaa > 1 ? 0 : 1][t.ssr ? 2 : (t.dofMrt ? 1 : 0)]);
                oe_line_params_t u{};
                Put(u.view_proj, viewProj);
                sg_apply_uniforms(UB_oe_line_params, SG_RANGE(u));
                sg_bindings b{};
                b.vertex_buffers[0] = lines.buf;
                sg_apply_bindings(&b);
                sg_draw(0, static_cast<int>(lineVerts.size()), 1);
            }
            sg_end_pass();
        }

        // ----- Depth of field: the depth of opaque and cut-out surfaces as a color (single sample;
        // transparent surfaces write no depth, as in the scene pass), then two blur passes.
        const PostProcess post = NormalizePostProcess(view.postProcess);
        if (t.ssr) {
            sg_pass pass{};
            pass.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
            pass.attachments.colors[0] = t.ssrColorAtt; pass.label = "oe-ssr";
            sg_begin_pass(&pass); Use(ssrPip);
            oe_ssr_params_t u{};
            Put(u.ssr_inverse, viewProj.Inverse()); Put(u.ssr_view, view.view); Put(u.ssr_projection, view.proj);
            Put(u.ssr_eye, view.eye.x, view.eye.y, view.eye.z, view.proj.at(3, 3) == 1 ? 1.0f : 0.0f);
            Put(u.ssr_lens, post.ssr, post.ssrDistance, post.ssrThickness, 0);
            sg_apply_uniforms(UB_oe_ssr_params, SG_RANGE(u));
            sg_bindings b{};
            b.views[VIEW_oe_ssr_scene_tex] = t.sceneTex; b.views[VIEW_oe_ssr_depth_tex] = t.ssaoDepthTex;
            b.views[VIEW_oe_ssr_normal_tex] = t.ssrTex[0]; b.views[VIEW_oe_ssr_weight_tex] = t.ssrTex[1];
            b.views[VIEW_oe_ssr_fallback_tex] = t.ssrTex[2]; b.samplers[SMP_oe_ssr_smp] = nearestClamp;
            sg_apply_bindings(&b); sg_draw(0, 3, 1); sg_end_pass();
        }
        if (t.dof) {
            // Without the scene pass's view-depth target: draw opaque and cut-out surfaces again for their depth.
            if (!t.dofMrt) drawDepth(t.dofDepthAtt);
            for (int stage = 0; stage < 2; ++stage) {
                sg_pass blur{};
                blur.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
                blur.attachments.colors[0] = stage == 0 ? t.dofHorizontalAtt : t.dofVerticalAtt;
                blur.label = stage == 0 ? "oe-dof-horizontal" : "oe-dof-vertical";
                sg_begin_pass(&blur);
                Use(dofPip[t.hdr ? 1 : 0]);
                oe_dof_params_t du{};
                Put(du.dof_lens, static_cast<float>(post.dofRadius), post.dofFocus, post.dofRange, post.dofFalloff);
                Put(du.dof_proj, view.proj.at(2, 2), view.proj.at(2, 3), view.proj.at(3, 3) == 1.0f ? 1.0f : 0.0f, static_cast<float>(stage));
                Put(du.dof_mode, t.dofMrt ? 1.0f : 0.0f, 0, 0, 0);
                sg_apply_uniforms(UB_oe_dof_params, SG_RANGE(du));
                sg_bindings b{};
                b.views[VIEW_oe_dof_source_tex] = stage == 0 ? (t.ssr ? t.ssrColorTex : t.sceneTex) : t.dofHorizontalTex;
                b.views[VIEW_oe_dof_depth_tex] = t.dofDepthTex;
                b.samplers[SMP_oe_dof_source_smp] = nearestClamp;
                b.samplers[SMP_oe_dof_depth_smp] = nearestClamp;
                sg_apply_bindings(&b);
                sg_draw(0, 3, 1);
                sg_end_pass();
            }
        }
        // Scene color the later effects read: blurred when depth of field is on.
        sg_view sceneSource = t.dof ? t.dofVerticalTex : (t.ssr ? t.ssrColorTex : t.sceneTex);

        // ----- Selection mask (editor): visible pixels of the highlighted entity
        bool outline = false;
        if (view.highlight != kNullEntity) {
            for (const RenderItem& it : items) outline = outline || it.id == view.highlight;
        }
        if (outline) {
            // Games have no selection. Avoid the unused depth pass and its GLES
            // framebuffer transition from multisampled to single-sampled attachments.
            sg_pass pass{};
            pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
            pass.action.colors[0].clear_value = {0, 0, 0, 0};
            pass.action.depth.load_action = SG_LOADACTION_CLEAR;
            pass.action.depth.clear_value = 1.0f;
            pass.attachments.colors[0] = t.maskAtt;
            pass.attachments.depth_stencil = t.maskDepthAtt;
            pass.label = "oe-mask";
            sg_begin_pass(&pass);
            if (outline) {
                for (int stage = 0; stage < 2; ++stage) {
                    Use(stage == 0 ? maskDepthPip : maskDrawPip);
                    oe_solid_params_t su{};
                    Put(su.color, 1, 1, 1, 1);
                    sg_apply_uniforms(UB_oe_solid_params, SG_RANGE(su));
                    for (size_t i = 0; i < items.size(); ++i) {
                        if (stage == 1 && items[i].id != view.highlight) continue;
                        bool surfaceAlpha = false;
                        for (const DrawCall& draw : draws) if (draw.item == i && draw.material.shader && draw.pass == 0)
                            surfaceAlpha = surfaceAlpha || draw.blend || draw.material.alphaMode == AlphaMode::Mask || Displaced(draw.material);
                        if (surfaceAlpha) {
                            for (const DrawCall& draw : draws) {
                                if (draw.item != i || draw.pass > 0) continue;
                                const sg_pipeline standard = surfaceMask[stage][draw.material.doubleSided ? 1 : 0];
                                const sg_pipeline pipeline = GraphPipeline(standard, draw.material);
                                Use(pipeline);
                                const float cutoff = draw.material.alphaMode == AlphaMode::Mask ? draw.material.alphaCutoff :
                                                     (draw.blend ? 0.5f : 0.0f);
                                ApplySurface(draw, items[i], Binding(*drawMeshes[static_cast<size_t>(&draw - draws.data())], items[i].mesh->submeshes[draw.submesh]), viewProj, view.shaderTime, cutoff, pipeline, standard);
                                const Submesh& sub = items[i].mesh->submeshes[draw.submesh];
                                sg_draw(static_cast<int>(sub.firstIndex), static_cast<int>(sub.indexCount), 1);
                            }
                            continue;
                        }
                        Use(stage == 0 ? maskDepthPip : maskDrawPip);
                        sg_apply_uniforms(UB_oe_solid_params, SG_RANGE(su));
                        oe_pos_vs_params_t u{};
                        Put(u.mvp, viewProj * items[i].world);
                        sg_apply_uniforms(UB_oe_pos_vs_params, SG_RANGE(u));
                        sg_bindings b{};
                        b.vertex_buffers[0] = gpuMeshes[i]->vbuf;
                        b.index_buffer = gpuMeshes[i]->ibuf;
                        sg_apply_bindings(&b);
                        const oe::Mesh& whole = *items[i].mesh;
                        if (items[i].joints.size() > oe::Mesh::kDrawJoints) {
                            for (size_t s = 0; s < whole.submeshes.size(); ++s) {  // a palette per range
                                ApplySkin(UB_oe_pos_vs_skin, items[i], static_cast<int>(s));
                                sg_draw(static_cast<int>(whole.submeshes[s].firstIndex), static_cast<int>(whole.submeshes[s].indexCount), 1);
                            }
                            continue;
                        }
                        ApplySkin(UB_oe_pos_vs_skin, items[i], -1);
                        sg_draw(0, static_cast<int>(whole.indices.size()), 1);
                    }
                }
            }
            sg_end_pass();
        }

        // ----- UI geometry (full output resolution)
        // Consecutive quads with the same texture and filter share a draw call.
        struct UIBatch {
            sg_view tex;
            sg_sampler smp;
            int first, count;
        };
        std::vector<UIVertex> uiVerts;
        std::vector<UIBatch> uiBatches;
        if (view.drawUI) {
            for (const UIQuad& q : BuildUIQuads(scene, outW, outH, assets)) {
                float x0 = static_cast<float>(q.x0), y0 = static_cast<float>(q.y0);
                float x1 = static_cast<float>(q.x1), y1 = static_cast<float>(q.y1);
                float tw = q.texture ? static_cast<float>(q.texture->width) : 1.0f, th = q.texture ? static_cast<float>(q.texture->height) : 1.0f;
                float u0 = q.s0 / tw, v0 = q.t0 / th, u1 = q.s1 / tw, v1 = q.t1 / th;
                UIVertex base{};
                Put(base.color, q.color.r, q.color.g, q.color.b, q.alpha);
                Put(base.border, q.borderColor.r, q.borderColor.g, q.borderColor.b, q.borderAlpha);
                Put(base.rect, q.shape[0], q.shape[1], q.shape[2], q.shape[3]);
                Put(base.params, q.radius, q.border, q.texture ? 1.0f : 0.0f, q.shaped ? 1.0f : 0.0f);
                const float corners[6][4] = {{x0, y0, u0, v0}, {x1, y0, u1, v0}, {x1, y1, u1, v1}, {x0, y0, u0, v0}, {x1, y1, u1, v1}, {x0, y1, u0, v1}};
                for (const auto& p : corners) {
                    UIVertex v = base;
                    v.pos[0] = p[0];
                    v.pos[1] = p[1];
                    v.uv[0] = p[2];
                    v.uv[1] = p[3];
                    uiVerts.push_back(v);
                }
                sg_view tex = UITextureFor(q.texture);
                sg_sampler smp = q.nearest ? nearestClamp : linearClamp;
                if (!uiBatches.empty() && uiBatches.back().tex.id == tex.id && uiBatches.back().smp.id == smp.id) uiBatches.back().count += 6;
                else uiBatches.push_back({tex, smp, static_cast<int>(uiVerts.size()) - 6, 6});
            }
        }
        bool haveUI = ui.Write(uiVerts.data(), uiVerts.size() * sizeof(UIVertex));

        auto configurePost = [&](const PostGraph& postGraph, oe_composite_params_t& cu, sg_pipeline& composite) {
            const ShaderGraph& graph = *postGraph.graph;
            const sg_pipeline compiled = PostPipeline(postGraph.graph);
            const bool interpretable = graph.instructions.size() <= static_cast<size_t>(ShaderGraph::kMaxInterpretedPostNodes);
            stats.postGraph = compiled.id ? "compiled" : interpretable ? "interpreted" : "skipped";
            if (compiled.id || interpretable) {
                Put(cu.post_graph, static_cast<float>(graph.instructions.size()), static_cast<float>(graph.color),
                    graph.beforeToneMapping ? 1.0f : 0.0f, view.shaderTime);
                if (compiled.id) {
                    composite = compiled;
                } else {
                    for (size_t node = 0; node < graph.instructions.size(); ++node) {
                        const ShaderInstruction& ins = graph.instructions[node];
                        Put(cu.post_code[node], static_cast<float>(ins.op), static_cast<float>(ins.args[0]),
                            static_cast<float>(ins.args[1]), static_cast<float>(ins.args[2]));
                        Put(cu.post_value[node], ins.value.x, ins.value.y, ins.value.z, ins.value.w);
                    }
                }
                for (size_t slot = 0; slot < postGraph.uniforms.size(); ++slot)
                    Put(cu.post_uniform[slot], postGraph.uniforms[slot].x, postGraph.uniforms[slot].y,
                        postGraph.uniforms[slot].z, postGraph.uniforms[slot].w);
            }
        };

        // A beforeToneMapping post graph: a full-screen pass from the scene into postColor (RGBA16F), which then
        // stands in for the scene in bloom and the composite. post_stage.x without FXAA makes DisplayColor return
        // the raw scene; a graph that cannot run yet (still compiling, too long to interpret) copies it through.
        if (t.beforeGraph) {
            const PostGraph graph = ResolvePostGraph(post, assets);
            oe_composite_params_t cu{};
            Put(cu.target, static_cast<float>(t.width), static_cast<float>(t.height), 0, 0);
            Put(cu.post_stage, 1, 0, 1.0f / static_cast<float>(t.width), 1.0f / static_cast<float>(t.height));
            sg_pipeline pipeline = postScenePip;
            configurePost(graph, cu, pipeline);
            sg_pass pass{};
            pass.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
            pass.attachments.colors[0] = t.postAtt;
            pass.label = "oe-post-scene";
            sg_begin_pass(&pass);
            Use(pipeline);
            sg_apply_uniforms(UB_oe_composite_params, SG_RANGE(cu));
            sg_bindings b{};
            b.views[VIEW_oe_scene_tex] = sceneSource;
            b.views[VIEW_oe_mask_tex] = whiteTex;
            b.views[VIEW_oe_bloom_tex] = whiteTex;
            b.samplers[SMP_oe_scene_smp] = linearClamp;
            b.samplers[SMP_oe_mask_smp] = nearestClamp;
            b.samplers[SMP_oe_bloom_smp] = linearClamp;
            sg_apply_bindings(&b);
            sg_draw(0, 3, 1);
            sg_end_pass();
            sceneSource = t.postTex;
        }

        // Two separable passes: horizontal highlight extraction/blur, then vertical
        // blur. UI and the selection mask are never sampled for bloom.
        if (t.bloom) for (int stage = 0; stage < 2; ++stage) {
            sg_pass pass{};
            pass.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
            pass.attachments.colors[0] = stage == 0 ? t.bloomHorizontalAtt : t.bloomVerticalAtt;
            pass.label = stage == 0 ? "oe-bloom-horizontal" : "oe-bloom-vertical";
            sg_begin_pass(&pass);
            Use(bloomPip);
            oe_bloom_params_t bu{};
            Put(bu.bloom_filter, static_cast<float>(post.bloomRadius), post.bloomThreshold, static_cast<float>(stage), 0);
            sg_apply_uniforms(UB_oe_bloom_params, SG_RANGE(bu));
            sg_bindings b{};
            b.views[VIEW_oe_bloom_source_tex] = stage == 0 ? sceneSource : t.bloomHorizontalTex;
            b.samplers[SMP_oe_bloom_source_smp] = nearestClamp;
            sg_apply_bindings(&b);
            sg_draw(0, 3, 1);
            sg_end_pass();
        }

        // Linear lighting: the composite encodes the float scene as sRGB after tone mapping (ColorSpace.h).
        const float linearOut = t.hdr && lights.linear ? 1.0f : 0.0f;
        // Quantize display color before FXAA/the post graph, matching the software renderer's RGBA8 source.
        if (t.fxaa) {
            sg_pass pass{};
            pass.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
            pass.attachments.colors[0] = t.fxaaAtt;
            pass.label = "oe-fxaa-source";
            sg_begin_pass(&pass);
            Use(compositePip);
            oe_composite_params_t cu{};
            Put(cu.target, static_cast<float>(t.width), static_cast<float>(t.height), 0, 0);
            Put(cu.bloom_strength, post.bloom, 0, 0, 0);
            Put(cu.tone_mapping, post.exposure, ToneMappingMode(post), linearOut, 0);
            Put(cu.vignette, post.vignette, post.vignetteRadius, post.vignetteSoftness, 0);
            Put(cu.vignette_color, post.vignetteColor.r, post.vignetteColor.g, post.vignetteColor.b, 0);
            Put(cu.grade_shadow, post.shadowTint.r, post.shadowTint.g, post.shadowTint.b, UsesGrading(post) ? 1.0f : 0.0f);
            Put(cu.grade_high, post.highlightTint.r, post.highlightTint.g, post.highlightTint.b, 0);
            Put(cu.grade_params, post.saturation, post.contrast, 0, 0);
            sg_apply_uniforms(UB_oe_composite_params, SG_RANGE(cu));
            sg_bindings b{};
            b.views[VIEW_oe_scene_tex] = sceneSource;
            b.views[VIEW_oe_mask_tex] = whiteTex;
            b.views[VIEW_oe_bloom_tex] = t.bloom ? t.bloomVerticalTex : whiteTex;
            b.samplers[SMP_oe_scene_smp] = linearClamp;
            b.samplers[SMP_oe_mask_smp] = nearestClamp;
            b.samplers[SMP_oe_bloom_smp] = linearClamp;
            sg_apply_bindings(&b);
            sg_draw(0, 3, 1);
            sg_end_pass();
        }

        // ----- Composite + UI into the output
        {
            sg_pass pass{};
            pass.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
            if (swapchain) pass.swapchain = *swapchain;
            else pass.attachments.colors[0] = t.outputAtt;
            pass.label = "oe-composite";
            sg_begin_pass(&pass);
            oe_composite_params_t cu{};
            Put(cu.target, static_cast<float>(outW), static_cast<float>(outH), 0, 0);
            Put(cu.outline_color, kOutlineColor.r, kOutlineColor.g, kOutlineColor.b, outline ? 1.0f : 0.0f);
            Put(cu.bloom_strength, post.bloom, 0, 0, 0);
            Put(cu.tone_mapping, post.exposure, ToneMappingMode(post), linearOut, 0);
            Put(cu.vignette, post.vignette, post.vignetteRadius, post.vignetteSoftness, 0);
            Put(cu.vignette_color, post.vignetteColor.r, post.vignetteColor.g, post.vignetteColor.b, 0);
            Put(cu.grade_shadow, post.shadowTint.r, post.shadowTint.g, post.shadowTint.b, UsesGrading(post) ? 1.0f : 0.0f);
            Put(cu.grade_high, post.highlightTint.r, post.highlightTint.g, post.highlightTint.b, 0);
            Put(cu.grade_params, post.saturation, post.contrast, 0, 0);
            Put(cu.post_stage, t.fxaa ? 1.0f : 0.0f, t.fxaa && post.fxaa ? 1.0f : 0.0f,
                1.0f / static_cast<float>(t.width), 1.0f / static_cast<float>(t.height));
            // The post graph runs in its own compiled composite program; until that is ready (or if it
            // failed) composite_fs interprets it from its instructions as uniforms, up to kMaxInterpretedPostNodes.
            const PostGraph postGraph = ResolvePostGraph(post, assets);
            sg_pipeline composite = compositePip;
            if (t.fxaa && postGraph.graph && !postGraph.graph->beforeToneMapping) {
                configurePost(postGraph, cu, composite);
            }
            Use(composite);
            sg_apply_uniforms(UB_oe_composite_params, SG_RANGE(cu));
            sg_bindings b{};
            b.views[VIEW_oe_scene_tex] = t.fxaa ? t.fxaaTex : sceneSource;
            b.views[VIEW_oe_mask_tex] = outline ? t.maskTex : whiteTex;
            b.views[VIEW_oe_bloom_tex] = t.bloom ? t.bloomVerticalTex : whiteTex;
            b.samplers[SMP_oe_scene_smp] = linearClamp;
            b.samplers[SMP_oe_mask_smp] = nearestClamp;
            b.samplers[SMP_oe_bloom_smp] = linearClamp;
            sg_apply_bindings(&b);
            sg_draw(0, 3, 1);
            if (haveUI) {
                Use(uiPip);
                oe_ui_params_t uu{};
                Put(uu.screen, 2.0f / static_cast<float>(outW), 2.0f / static_cast<float>(outH), 0, 0);
                sg_apply_uniforms(UB_oe_ui_params, SG_RANGE(uu));
                for (const UIBatch& batch : uiBatches) {
                    sg_bindings ub{};
                    ub.vertex_buffers[0] = ui.buf;
                    ub.views[VIEW_oe_ui_tex] = batch.tex;
                    ub.samplers[SMP_oe_ui_smp] = batch.smp;
                    sg_apply_bindings(&ub);
                    sg_draw(batch.first, batch.count, 1);
                }
            }
            sg_end_pass();
        }
        sg_commit();
        stats.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        return stats;
    }
    RenderStats PixelFrame(const Scene& scene, const RenderView& view, Targets& output, int outW, int outH, const sg_swapchain* swapchain) {
        RenderView low = view;
        low.pixelWidth = low.pixelHeight = 0;
        low.drawUI = false;
        PrepareTargets(pixelSource, view.pixelWidth, view.pixelHeight, true, low, true);
        RenderStats stats = Frame(scene, low, pixelSource, view.pixelWidth, view.pixelHeight, nullptr);
        sg_pass pass{};
        pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
        pass.action.colors[0].clear_value = {0, 0, 0, 1};
        if (swapchain)
            pass.swapchain = *swapchain;
        else
            pass.attachments.colors[0] = output.outputAtt;
        sg_begin_pass(&pass);
        Use(compositePip);
        const int scale = std::max(1, std::min(outW / view.pixelWidth, outH / view.pixelHeight));
        const int width = view.pixelWidth * scale, height = view.pixelHeight * scale;
        sg_apply_viewport((outW - width) / 2, (outH - height) / 2, width, height, true);
        oe_composite_params_t cu{};
        Put(cu.target, static_cast<float>(width), static_cast<float>(height), static_cast<float>((outW - width) / 2),
            static_cast<float>((outH - height) / 2));
        Put(cu.tone_mapping, 1, 0, 0, 0);
        Put(cu.grade_params, 1, 1, 0, 0);
        Put(cu.post_stage, 1, 0, 0, 0);
        sg_apply_uniforms(UB_oe_composite_params, SG_RANGE(cu));
        sg_bindings b{};
        b.views[VIEW_oe_scene_tex] = pixelSource.outputTex;
        b.views[VIEW_oe_mask_tex] = whiteTex;
        b.views[VIEW_oe_bloom_tex] = whiteTex;
        b.samplers[SMP_oe_scene_smp] = nearestClamp;
        b.samplers[SMP_oe_mask_smp] = nearestClamp;
        b.samplers[SMP_oe_bloom_smp] = nearestClamp;
        sg_apply_bindings(&b);
        sg_draw(0, 3, 1);
        if (view.drawUI) {
            sg_apply_viewport(0, 0, outW, outH, true);
            Use(uiPip);
            oe_ui_params_t params{};
            Put(params.screen, 2.0f / static_cast<float>(outW), 2.0f / static_cast<float>(outH), 0, 0);
            sg_apply_uniforms(UB_oe_ui_params, SG_RANGE(params));
            for (const UIQuad& q : BuildUIQuads(scene, outW, outH, assets)) {
                const float tw = q.texture ? static_cast<float>(q.texture->width) : 1.0f,
                            th = q.texture ? static_cast<float>(q.texture->height) : 1.0f;
                UIVertex base{};
                Put(base.color, q.color.r, q.color.g, q.color.b, q.alpha);
                Put(base.border, q.borderColor.r, q.borderColor.g, q.borderColor.b, q.borderAlpha);
                Put(base.rect, q.shape[0], q.shape[1], q.shape[2], q.shape[3]);
                Put(base.params, q.radius, q.border, q.texture ? 1.0f : 0.0f, q.shaped ? 1.0f : 0.0f);
                const float x0 = static_cast<float>(q.x0), y0 = static_cast<float>(q.y0), x1 = static_cast<float>(q.x1),
                            y1 = static_cast<float>(q.y1);
                const float corners[6][4] = {{x0, y0, q.s0 / tw, q.t0 / th}, {x1, y0, q.s1 / tw, q.t0 / th}, {x1, y1, q.s1 / tw, q.t1 / th},
                                             {x0, y0, q.s0 / tw, q.t0 / th}, {x1, y1, q.s1 / tw, q.t1 / th}, {x0, y1, q.s0 / tw, q.t1 / th}};
                UIVertex vertices[6];
                for (int i = 0; i < 6; ++i) {
                    vertices[i] = base;
                    vertices[i].pos[0] = corners[i][0];
                    vertices[i].pos[1] = corners[i][1];
                    vertices[i].uv[0] = corners[i][2];
                    vertices[i].uv[1] = corners[i][3];
                }
                if (!ui.Write(vertices, sizeof(vertices))) continue;
                sg_bindings ub{};
                ub.vertex_buffers[0] = ui.buf;
                ub.views[VIEW_oe_ui_tex] = UITextureFor(q.texture);
                ub.samplers[SMP_oe_ui_smp] = q.nearest ? nearestClamp : linearClamp;
                sg_apply_bindings(&ub);
                sg_draw(0, 6, 1);
            }
        }
        sg_end_pass();
        sg_commit();
        return stats;
    }
};

GpuRenderer::GpuRenderer(GpuDevice& device, AssetManager* assets, const Settings& settings) : device_(device) {
    sg_desc desc{};
    desc.environment = device.Environment();
    desc.logger.func = SgLog;
    desc.buffer_pool_size = 4096;
    desc.image_pool_size = 1024;
    desc.view_pool_size = 2048;
    desc.sampler_pool_size = 64;
    desc.shader_pool_size = 256;
    desc.pipeline_pool_size = 1024;
    desc.uniform_buffer_size = 8 * 1024 * 1024;
    sg_setup(&desc);
    Settings s = settings;
    s.msaa = s.msaa >= 4 ? 4 : (s.msaa >= 2 ? 2 : 1);
    s.shadowMapSize = std::clamp(s.shadowMapSize, 256, 4096);
    impl_ = std::make_unique<Impl>(device, assets, s);
    impl_->Init();
    name_ = std::string("gpu (") + device.Name() + ")";
}

GpuRenderer::~GpuRenderer() {
    impl_->Shutdown();
    impl_.reset();
    sg_shutdown();
}

RenderStats GpuRenderer::Render(const Scene& scene, const RenderView& view, RenderTarget& target) {
    int w = std::max(1, target.width), h = std::max(1, target.height);
    if (target.color.size() != static_cast<size_t>(w) * static_cast<size_t>(h)) target.Resize(w, h);
    Targets& t = impl_->offscreen;
    impl_->PrepareTargets(t, w, h, true, view);
    RenderStats stats = view.pixelWidth > 0 ? impl_->PixelFrame(scene, view, t, w, h, nullptr) : impl_->Frame(scene, view, t, w, h, nullptr);
    if (!device_.ReadPixels(t.output, w, h, target.color.data())) OE_LOG_ERROR("gpu", "reading back the frame failed");
    std::fill(target.depth.begin(), target.depth.end(), 1.0f);
    std::fill(target.ids.begin(), target.ids.end(), kNullEntity);
    return stats;
}

sg_view GpuRenderer::ImageView(const std::shared_ptr<const Texture>& texture) { return impl_->UITextureFor(texture); }
void GpuRenderer::CollectGarbage() { impl_->Evict(); }

void GpuRenderer::Preload(const std::vector<std::shared_ptr<const Material>>& materials,
                          const std::vector<std::shared_ptr<const Texture>>& textures,
                          const std::vector<std::shared_ptr<const Mesh>>& meshes) {
    // Loose textures are most likely color images; a data use uploads its own copy when it is drawn.
    const bool linear = impl_->assets && impl_->assets->LinearColor();
    for (const auto& texture : textures) impl_->ColorTextureFor(texture, linear);
    for (const auto& mesh : meshes)
        if (mesh && !mesh->positions.empty()) impl_->MeshFor(mesh, false);
    for (const auto& material : materials) {
        if (!material) continue;
        for (const auto& texture : {material->normalTexture, material->metallicRoughnessTexture, material->occlusionTexture})
            impl_->TextureFor(texture);
        for (const auto& texture : {material->baseTexture, material->emissiveTexture}) impl_->ColorTextureFor(texture, linear);
        // The material's own graph and the outline graph its shell draws with.
        std::vector<Material> graphs;
        if (material->shader) graphs.push_back(*material);
        if (material->outlineWidth > 0.0f) graphs.push_back(OutlineMaterial(*material));
        for (const Material* next = material->nextPass.get(); next; next = next->nextPass.get())
            if (next->shader) graphs.push_back(*next);
        // Every way the scene pass can draw it: a particle or a sprite blends a material that is opaque
        // on a mesh, and the targets depend on the camera's effects.
        for (const Material& graph : graphs)
            for (auto* pipelines : {&impl_->meshPips, &impl_->hdrMeshPips})
                for (auto& bySamples : *pipelines)
                    for (auto& byBlend : bySamples)
                        for (auto& bySide : byBlend)
                            for (sg_pipeline standard : bySide)
                                if (standard.id) impl_->GraphPipeline(standard, graph);
    }
}

sg_view GpuRenderer::RenderToTexture(const Scene& scene, const RenderView& view, int width, int height, int slot, RenderStats* stats) {
    int w = std::max(1, width), h = std::max(1, height);
    Targets& t = impl_->panelTargets[slot];
    impl_->PrepareTargets(t, w, h, true, view);
    RenderStats s = view.pixelWidth > 0 ? impl_->PixelFrame(scene, view, t, w, h, nullptr) : impl_->Frame(scene, view, t, w, h, nullptr);
    // The caller draws this image later in its own frame (editor panels): start the GPU on it now.
    device_.Flush();
    if (stats) *stats = s;
    return t.outputTex;
}

bool GpuRenderer::ReadTexture(int slot, RenderTarget& target) {
    auto it = impl_->panelTargets.find(slot);
    if (it == impl_->panelTargets.end() || it->second.width == 0) return false;
    const Targets& t = it->second;
    target.Resize(t.width, t.height);
    return device_.ReadPixels(t.output, t.width, t.height, target.color.data());
}

void GpuRenderer::WindowSize(int* width, int* height) {
    sg_swapchain sc = device_.Swapchain();
    *width = sc.invalid ? 0 : sc.width;
    *height = sc.invalid ? 0 : sc.height;
}

bool GpuRenderer::RenderToWindow(const Scene& scene, const RenderView& view, float renderScale, RenderStats* stats) {
    sg_swapchain sc = device_.Swapchain();
    if (sc.invalid || sc.width <= 0 || sc.height <= 0) return false;
    float scale = std::clamp(renderScale, 0.25f, 1.0f);
    int w = std::max(1, static_cast<int>(std::lround(static_cast<float>(sc.width) * scale)));
    int h = std::max(1, static_cast<int>(std::lround(static_cast<float>(sc.height) * scale)));
    Targets& t = impl_->window;
    impl_->PrepareTargets(t, w, h, false, view);
    RenderStats s =
        view.pixelWidth > 0 ? impl_->PixelFrame(scene, view, t, sc.width, sc.height, &sc) : impl_->Frame(scene, view, t, sc.width, sc.height, &sc);
    device_.Present();
    if (stats) *stats = s;
    return true;
}

}  // namespace oe
