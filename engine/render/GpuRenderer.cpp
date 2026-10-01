#include "render/GpuRenderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "core/Log.h"
#include "render/GpuDevice.h"
#include "render/RenderScene.h"
#include "render/UI.h"
#include "render/shaders/Shaders.glsl.h"
#include "scene/Components.h"

namespace oe {

namespace {

constexpr sg_pixel_format kColorFormat = SG_PIXELFORMAT_RGBA8;
constexpr sg_pixel_format kDepthFormat = SG_PIXELFORMAT_DEPTH;
constexpr int kMaxDirLights = 4;
constexpr int kMaxPointLights = 16;
const Color kOutlineColor(1.0f, 0.62f, 0.1f);

struct MeshVertex {
    float pos[3];
    float nrm[3];
    float uv[2];
    float tan[4];
    float joints[4];
    float weights[4];
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

// Pixel format of Texture::texels (bytes R, G, B, A) is RGBA8. Mip levels
// are box filtered on the CPU so distant textures do not shimmer.
std::vector<std::vector<uint32_t>> BuildMips(const Texture& tex) {
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
                for (int k = 0; k < 4; ++k) {
                    int sx = std::min(w - 1, x * 2 + (k & 1)), sy = std::min(h - 1, y * 2 + (k >> 1));
                    uint32_t c = src[static_cast<size_t>(sy) * static_cast<size_t>(w) + static_cast<size_t>(sx)];
                    for (int ch = 0; ch < 4; ++ch) sum[ch] += (c >> (8 * ch)) & 0xFF;
                }
                uint32_t out = 0;
                for (int ch = 0; ch < 4; ++ch) out |= ((sum[ch] + 2) / 4) << (8 * ch);
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

// A streamed vertex buffer rewritten every frame (lines, UI).
struct StreamBuffer {
    sg_buffer buf{};
    size_t capacity = 0;
    // Writes `bytes` and returns false when there is nothing to draw.
    bool Write(const void* data, size_t bytes) {
        if (bytes == 0) return false;
        if (bytes > capacity) {
            if (capacity) sg_destroy_buffer(buf);
            capacity = std::max<size_t>(bytes, std::max<size_t>(capacity * 2, 64 * 1024));
            sg_buffer_desc d{};
            d.size = capacity;
            d.usage.vertex_buffer = true;
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
    sg_image sceneMsaa{}, sceneDepth{}, sceneColor{}, maskColor{}, maskDepth{}, output{};
    sg_view sceneMsaaAtt{}, sceneDepthAtt{}, sceneColorAtt{}, sceneTex{}, maskAtt{}, maskDepthAtt{}, maskTex{}, outputAtt{}, outputTex{};

    bool Matches(int w, int h, bool output_) const { return width == w && height == h && withOutput == output_; }

    void Create(int w, int h, int samples, bool output_) {
        Destroy();
        width = w;
        height = h;
        msaa = samples;
        withOutput = output_;
        if (msaa > 1) {
            sceneMsaa = MakeAttachmentImage(w, h, kColorFormat, msaa, false, false, "oe-scene-msaa");
            sceneMsaaAtt = ColorView(sceneMsaa);
            sceneColor = MakeAttachmentImage(w, h, kColorFormat, 1, false, true, "oe-scene");
            sceneColorAtt = ResolveView(sceneColor);
        } else {
            sceneColor = MakeAttachmentImage(w, h, kColorFormat, 1, false, false, "oe-scene");
            sceneColorAtt = ColorView(sceneColor);
        }
        sceneDepth = MakeAttachmentImage(w, h, kDepthFormat, msaa, true, false, "oe-scene-depth");
        sceneDepthAtt = DepthView(sceneDepth);
        sceneTex = TextureView(sceneColor);
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
        for (sg_view v : {sceneMsaaAtt, sceneDepthAtt, sceneColorAtt, sceneTex, maskAtt, maskDepthAtt, maskTex, outputAtt, outputTex}) {
            if (v.id) sg_destroy_view(v);
        }
        for (sg_image i : {sceneMsaa, sceneDepth, sceneColor, maskColor, maskDepth, output}) {
            if (i.id) sg_destroy_image(i);
        }
        *this = Targets();
    }
};

void SgLog(const char* tag, uint32_t level, uint32_t item, const char* message, uint32_t line, const char*, void*) {
    const char* msg = message ? message : "(no message in release builds)";
    if (level <= 1) OE_LOG_ERROR("gpu", "%s item %u line %u: %s", tag, item, line, msg);
    else if (level == 2) OE_LOG_WARN("gpu", "%s item %u line %u: %s", tag, item, line, msg);
}

}  // namespace

struct GpuRenderer::Impl {
    GpuDevice& device;
    AssetManager* assets;
    Settings settings;

    sg_pipeline meshPips[2][2]{};  // [blend][double sided]
    sg_pipeline shadowPip{}, shadowPipTwoSided{}, maskDepthPip{}, maskDrawPip{}, linePip{}, compositePip{}, uiPip{};
    sg_shader meshShd{}, shadowShd{}, solidShd{}, lineShd{}, compositeShd{}, uiShd{};
    sg_sampler linearRepeat{}, linearClamp{}, nearestClamp{}, pixelArt{}, shadowCompare{};
    sg_image white{}, shadowMap{};
    sg_view whiteTex{}, shadowAtt{}, shadowTex{};
    StreamBuffer lines, ui;
    Targets offscreen, window;
    std::unordered_map<int, Targets> panelTargets;  // RenderToTexture slots (editor panels)

    std::unordered_map<const Mesh*, GpuMesh> meshes;
    std::unordered_map<const Mesh*, GpuMesh> flatMeshes;
    std::unordered_map<const Texture*, GpuTexture> textures;
    std::unordered_map<const Texture*, GpuTexture> uiTextures;  // no mips; re-uploaded when Texture::version changes

    Impl(GpuDevice& d, AssetManager* a, const Settings& s) : device(d), assets(a), settings(s) {}

    void Init() {
        sg_backend backend = sg_query_backend();
        meshShd = sg_make_shader(oe_mesh_shader_desc(backend));
        shadowShd = sg_make_shader(oe_shadow_shader_desc(backend));
        solidShd = sg_make_shader(oe_solid_shader_desc(backend));
        lineShd = sg_make_shader(oe_line_shader_desc(backend));
        compositeShd = sg_make_shader(oe_composite_shader_desc(backend));
        uiShd = sg_make_shader(oe_ui_shader_desc(backend));

        const int msaa = settings.msaa;
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
        // Opaque/cut-out surfaces write depth; transparent ones blend and only test it.
        for (int blend = 0; blend < 2; ++blend) {
            for (int twoSided = 0; twoSided < 2; ++twoSided) {
                sg_pipeline_desc d{};
                d.shader = meshShd;
                d.layout.buffers[0].stride = sizeof(MeshVertex);
                d.layout.attrs[ATTR_oe_mesh_position] = {0, offsetof(MeshVertex, pos), SG_VERTEXFORMAT_FLOAT3};
                d.layout.attrs[ATTR_oe_mesh_normal] = {0, offsetof(MeshVertex, nrm), SG_VERTEXFORMAT_FLOAT3};
                d.layout.attrs[ATTR_oe_mesh_texcoord] = {0, offsetof(MeshVertex, uv), SG_VERTEXFORMAT_FLOAT2};
                d.layout.attrs[ATTR_oe_mesh_tangent] = {0, offsetof(MeshVertex, tan), SG_VERTEXFORMAT_FLOAT4};
                d.layout.attrs[ATTR_oe_mesh_joint_indices] = {0, offsetof(MeshVertex, joints), SG_VERTEXFORMAT_FLOAT4};
                d.layout.attrs[ATTR_oe_mesh_joint_weights] = {0, offsetof(MeshVertex, weights), SG_VERTEXFORMAT_FLOAT4};
                d.index_type = SG_INDEXTYPE_UINT32;
                d.depth.pixel_format = kDepthFormat;
                d.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
                d.depth.write_enabled = blend == 0;
                d.colors[0].pixel_format = kColorFormat;
                if (blend) alphaBlend(d.colors[0]);
                d.cull_mode = twoSided ? SG_CULLMODE_NONE : SG_CULLMODE_BACK;
                d.face_winding = SG_FACEWINDING_CCW;
                d.sample_count = msaa;
                d.label = blend ? "oe-mesh-blend" : "oe-mesh";
                meshPips[blend][twoSided] = sg_make_pipeline(&d);
            }
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
        {
            sg_pipeline_desc d{};
            d.shader = lineShd;
            d.layout.buffers[0].stride = sizeof(ColorVertex);
            d.layout.attrs[ATTR_oe_line_position] = {0, offsetof(ColorVertex, pos), SG_VERTEXFORMAT_FLOAT3};
            d.layout.attrs[ATTR_oe_line_color0] = {0, offsetof(ColorVertex, color), SG_VERTEXFORMAT_FLOAT4};
            d.primitive_type = SG_PRIMITIVETYPE_LINES;
            d.depth.pixel_format = kDepthFormat;
            d.depth.compare = SG_COMPAREFUNC_LESS_EQUAL;
            alphaBlend(d.colors[0]);
            d.sample_count = msaa;
            d.label = "oe-lines";
            linePip = sg_make_pipeline(&d);
        }
        {
            sg_pipeline_desc d{};
            d.shader = compositeShd;
            d.depth.pixel_format = SG_PIXELFORMAT_NONE;
            d.colors[0].pixel_format = kColorFormat;
            d.sample_count = 1;
            d.label = "oe-composite";
            compositePip = sg_make_pipeline(&d);
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

    void Shutdown() {
        for (auto& kv : meshes) kv.second.Destroy();
        for (auto& kv : flatMeshes) kv.second.Destroy();
        for (auto& kv : textures) kv.second.Destroy();
        for (auto& kv : uiTextures) kv.second.Destroy();
        meshes.clear();
        flatMeshes.clear();
        textures.clear();
        uiTextures.clear();
        offscreen.Destroy();
        window.Destroy();
        for (auto& kv : panelTargets) kv.second.Destroy();
        panelTargets.clear();
        lines.Destroy();
        ui.Destroy();
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
        sweep(textures);
        sweep(uiTextures);
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
    // (the software renderer's flat shading), keeping the index ranges of submeshes.
    const GpuMesh& MeshFor(const std::shared_ptr<const oe::Mesh>& mesh, bool flat) {
        auto& map = flat ? flatMeshes : meshes;
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
            const Vec4 t = static_cast<size_t>(i) < m.tangents.size() ? m.tangents[i] : Vec4(1, 0, 0, 1);
            v.tan[0] = t.x;
            v.tan[1] = t.y;
            v.tan[2] = t.z;
            v.tan[3] = t.w;
            if (i < m.skin.size()) for (size_t k = 0; k < 4; ++k) {
                v.joints[k] = static_cast<float>(m.skin[i].joints[k]);
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
            for (size_t i = 0; i < m.positions.size(); ++i) {
                verts.push_back(vertex(static_cast<uint32_t>(i), i < m.normals.size() ? m.normals[i] : Vec3(0, 1, 0)));
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

    // Mipmapped GPU copy of a material texture; empty view when there is none.
    sg_view TextureFor(const std::shared_ptr<const Texture>& owner) {
        const Texture* tex = owner.get();
        if (!tex || tex->width <= 0 || tex->height <= 0) return sg_view{};
        auto found = textures.find(tex);
        if (found != textures.end() && OwnerAlive(found->second.permanent, found->second.owner)) return found->second.view;
        if (found != textures.end()) {
            found->second.Destroy();
            textures.erase(found);
        }
        std::vector<std::vector<uint32_t>> mips = BuildMips(*tex);
        sg_image_desc d{};
        d.width = tex->width;
        d.height = tex->height;
        d.pixel_format = kColorFormat;
        d.num_mipmaps = static_cast<int>(mips.size());
        for (size_t i = 0; i < mips.size(); ++i) d.data.mip_levels[i] = {mips[i].data(), mips[i].size() * sizeof(uint32_t)};
        d.label = "oe-texture";
        GpuTexture g;
        g.image = sg_make_image(&d);
        g.view = TextureView(g.image);
        g.owner = owner;
        return (textures[tex] = g).view;
    }

    // ----- Frame ------------------------------------------------------------------

    // Draws `scene` into `t` (scene + mask images) and composites into the
    // output: the offscreen output image or the window swapchain.
    RenderStats Frame(const Scene& scene, const RenderView& view, Targets& t, int outW, int outH, const sg_swapchain* swapchain) {
        auto start = std::chrono::steady_clock::now();
        RenderStats stats;
        Evict();
        std::vector<RenderItem> items = GatherRenderItems(scene, assets, view.view);
        RenderLights lights = GatherRenderLights(scene);
        const std::vector<DrawCall> draws = BuildDrawList(items, view.eye);
        std::vector<const GpuMesh*> gpuMeshes;
        gpuMeshes.reserve(items.size());
        for (const RenderItem& it : items) {
            gpuMeshes.push_back(&MeshFor(it.mesh, it.flat));
            stats.triangles += static_cast<int>(it.mesh->TriangleCount());
        }
        stats.drawnEntities = static_cast<int>(items.size());

        // ----- Shadow map: first directional light, fitted to casters + receivers
        bool shadows = false;
        ShadowFit fit;
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
            if (anyCaster && lo.x <= hi.x) {
                shadows = true;
                fit = FitShadow(lo, hi, lights.dirs[0].dir, settings.shadowMapSize);
            }
        }
        if (shadows) {
            sg_pass pass{};
            pass.action.depth.load_action = SG_LOADACTION_CLEAR;
            pass.action.depth.clear_value = 1.0f;
            pass.action.depth.store_action = SG_STOREACTION_STORE;
            pass.attachments.depth_stencil = shadowAtt;
            pass.label = "oe-shadow";
            sg_begin_pass(&pass);
            int current = -1;
            for (const DrawCall& dc : draws) {
                const RenderItem& it = items[dc.item];
                if (!it.castShadows || it.unlit || dc.blend) continue;  // transparent surfaces cast no shadow
                int want = dc.material.doubleSided ? 1 : 0;
                if (want != current) {
                    sg_apply_pipeline(want ? shadowPipTwoSided : shadowPip);
                    current = want;
                }
                oe_pos_vs_params_t u{};
                Put(u.mvp, fit.viewProj * it.world);
                for (size_t j = 0; j < it.joints.size(); ++j) Put(u.joint_palette[j], it.joints[j]);
                sg_apply_uniforms(UB_oe_pos_vs_params, SG_RANGE(u));
                sg_bindings b{};
                b.vertex_buffers[0] = gpuMeshes[dc.item]->vbuf;
                b.index_buffer = gpuMeshes[dc.item]->ibuf;
                sg_apply_bindings(&b);
                const Submesh& sub = it.mesh->submeshes[dc.submesh];
                sg_draw(static_cast<int>(sub.firstIndex), static_cast<int>(sub.indexCount), 1);
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
            lineVerts.push_back({{p.x, p.y, p.z}, {c.r, c.g, c.b, alpha}});
            lineVerts.push_back({{q.x, q.y, q.z}, {c.r, c.g, c.b, alpha}});
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

        // ----- Scene pass (MSAA, resolved into t.sceneColor)
        {
            sg_pass pass{};
            pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
            pass.action.colors[0].clear_value = {view.clearColor.r, view.clearColor.g, view.clearColor.b, 1.0f};
            pass.action.colors[0].store_action = t.msaa > 1 ? SG_STOREACTION_DONTCARE : SG_STOREACTION_STORE;
            pass.action.depth.load_action = SG_LOADACTION_CLEAR;
            pass.action.depth.clear_value = 1.0f;
            if (t.msaa > 1) {
                pass.attachments.colors[0] = t.sceneMsaaAtt;
                pass.attachments.resolves[0] = t.sceneColorAtt;
            } else {
                pass.attachments.colors[0] = t.sceneColorAtt;
            }
            pass.attachments.depth_stencil = t.sceneDepthAtt;
            pass.label = "oe-scene";
            sg_begin_pass(&pass);

            oe_mesh_lights_t lu{};
            Put(lu.shadow_vp, fit.viewProj);
            Put(lu.ambient, lights.ambient.r, lights.ambient.g, lights.ambient.b, 0);
            int dirs = std::min(kMaxDirLights, static_cast<int>(lights.dirs.size()));
            int points = std::min(kMaxPointLights, static_cast<int>(lights.points.size()));
            Put(lu.counts, static_cast<float>(dirs), static_cast<float>(points), shadows ? 1.0f : 0.0f, lights.shadowStrength);
            Put(lu.shadow_params, fit.texelWorld, 1.0f / static_cast<float>(settings.shadowMapSize), 0.002f, 0);
            Put(lu.eye, view.eye.x, view.eye.y, view.eye.z, 1);
            for (int i = 0; i < dirs; ++i) {
                const RenderDirLight& l = lights.dirs[static_cast<size_t>(i)];
                Put(lu.dir_dir[i], l.dir.x, l.dir.y, l.dir.z, 0);
                Put(lu.dir_color[i], l.color.r, l.color.g, l.color.b, 0);
            }
            for (int i = 0; i < points; ++i) {
                const RenderPointLight& l = lights.points[static_cast<size_t>(i)];
                Put(lu.point_pos[i], l.pos.x, l.pos.y, l.pos.z, l.range);
                Put(lu.point_color[i], l.color.r, l.color.g, l.color.b, 0);
            }
            // Draw list order: opaque, then transparent back to front. Switching
            // pipelines drops uniforms, so the lights are re-applied each time.
            sg_pipeline current{};
            for (const DrawCall& dc : draws) {
                const RenderItem& it = items[dc.item];
                const Material& m = dc.material;
                sg_pipeline pip = meshPips[dc.blend ? 1 : 0][m.doubleSided ? 1 : 0];
                if (pip.id != current.id) {
                    sg_apply_pipeline(pip);
                    sg_apply_uniforms(UB_oe_mesh_lights, SG_RANGE(lu));
                    current = pip;
                }
                oe_mesh_vs_params_t vu{};
                Put(vu.view_proj, viewProj);
                Put(vu.model, it.world);
                Put(vu.normal_mat, it.normalMatrix);
                for (size_t j = 0; j < it.joints.size(); ++j) Put(vu.joint_palette[j], it.joints[j]);
                sg_apply_uniforms(UB_oe_mesh_vs_params, SG_RANGE(vu));
                sg_view base = TextureFor(m.baseTexture);
                sg_view normal = m.unlit ? sg_view{} : TextureFor(m.normalTexture);
                sg_view mr = m.unlit ? sg_view{} : TextureFor(m.metallicRoughnessTexture);
                sg_view emissive = TextureFor(m.emissiveTexture);
                sg_view occlusion = m.unlit ? sg_view{} : TextureFor(m.occlusionTexture);
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
                sg_apply_uniforms(UB_oe_mesh_material, SG_RANGE(mu));
                sg_bindings b{};
                b.vertex_buffers[0] = gpuMeshes[dc.item]->vbuf;
                b.index_buffer = gpuMeshes[dc.item]->ibuf;
                b.views[VIEW_oe_base_tex] = base.id ? base : whiteTex;
                b.views[VIEW_oe_shadow_tex] = shadowTex;
                b.views[VIEW_oe_normal_tex] = normal.id ? normal : whiteTex;
                b.views[VIEW_oe_mr_tex] = mr.id ? mr : whiteTex;
                b.views[VIEW_oe_emissive_tex] = emissive.id ? emissive : whiteTex;
                b.views[VIEW_oe_occlusion_tex] = occlusion.id ? occlusion : whiteTex;
                b.samplers[SMP_oe_base_smp] = m.pixelArt ? pixelArt : linearRepeat;
                b.samplers[SMP_oe_shadow_smp] = shadowCompare;
                sg_apply_bindings(&b);
                const Submesh& sub = it.mesh->submeshes[dc.submesh];
                sg_draw(static_cast<int>(sub.firstIndex), static_cast<int>(sub.indexCount), 1);
            }

            if (haveLines) {
                sg_apply_pipeline(linePip);
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
                    sg_apply_pipeline(stage == 0 ? maskDepthPip : maskDrawPip);
                    oe_solid_params_t su{};
                    Put(su.color, 1, 1, 1, 1);
                    sg_apply_uniforms(UB_oe_solid_params, SG_RANGE(su));
                    for (size_t i = 0; i < items.size(); ++i) {
                        if (stage == 1 && items[i].id != view.highlight) continue;
                        oe_pos_vs_params_t u{};
                        Put(u.mvp, viewProj * items[i].world);
                        for (size_t j = 0; j < items[i].joints.size(); ++j) Put(u.joint_palette[j], items[i].joints[j]);
                        sg_apply_uniforms(UB_oe_pos_vs_params, SG_RANGE(u));
                        sg_bindings b{};
                        b.vertex_buffers[0] = gpuMeshes[i]->vbuf;
                        b.index_buffer = gpuMeshes[i]->ibuf;
                        sg_apply_bindings(&b);
                        sg_draw(0, static_cast<int>(items[i].mesh->indices.size()), 1);
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

        // ----- Composite + UI into the output
        {
            sg_pass pass{};
            pass.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
            if (swapchain) pass.swapchain = *swapchain;
            else pass.attachments.colors[0] = t.outputAtt;
            pass.label = "oe-composite";
            sg_begin_pass(&pass);
            sg_apply_pipeline(compositePip);
            oe_composite_params_t cu{};
            Put(cu.target, static_cast<float>(outW), static_cast<float>(outH), 0, 0);
            Put(cu.outline_color, kOutlineColor.r, kOutlineColor.g, kOutlineColor.b, outline ? 1.0f : 0.0f);
            sg_apply_uniforms(UB_oe_composite_params, SG_RANGE(cu));
            sg_bindings b{};
            b.views[VIEW_oe_scene_tex] = t.sceneTex;
            b.views[VIEW_oe_mask_tex] = outline ? t.maskTex : whiteTex;
            b.samplers[SMP_oe_scene_smp] = linearClamp;
            b.samplers[SMP_oe_mask_smp] = nearestClamp;
            sg_apply_bindings(&b);
            sg_draw(0, 3, 1);
            if (haveUI) {
                sg_apply_pipeline(uiPip);
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
};

GpuRenderer::GpuRenderer(GpuDevice& device, AssetManager* assets, const Settings& settings) : device_(device) {
    sg_desc desc{};
    desc.environment = device.Environment();
    desc.logger.func = SgLog;
    desc.buffer_pool_size = 4096;
    desc.image_pool_size = 1024;
    desc.view_pool_size = 2048;
    desc.sampler_pool_size = 64;
    desc.shader_pool_size = 64;
    desc.pipeline_pool_size = 64;
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
    if (!t.Matches(w, h, true)) t.Create(w, h, impl_->settings.msaa, true);
    RenderStats stats = impl_->Frame(scene, view, t, w, h, nullptr);
    if (!device_.ReadPixels(t.output, w, h, target.color.data())) OE_LOG_ERROR("gpu", "reading back the frame failed");
    std::fill(target.depth.begin(), target.depth.end(), 1.0f);
    std::fill(target.ids.begin(), target.ids.end(), kNullEntity);
    return stats;
}

sg_view GpuRenderer::ImageView(const std::shared_ptr<const Texture>& texture) { return impl_->UITextureFor(texture); }

sg_view GpuRenderer::RenderToTexture(const Scene& scene, const RenderView& view, int width, int height, int slot, RenderStats* stats) {
    int w = std::max(1, width), h = std::max(1, height);
    Targets& t = impl_->panelTargets[slot];
    if (!t.Matches(w, h, true)) t.Create(w, h, impl_->settings.msaa, true);
    RenderStats s = impl_->Frame(scene, view, t, w, h, nullptr);
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
    if (!t.Matches(w, h, false)) t.Create(w, h, impl_->settings.msaa, false);
    RenderStats s = impl_->Frame(scene, view, t, sc.width, sc.height, &sc);
    device_.Present();
    if (stats) *stats = s;
    return true;
}

}  // namespace oe
