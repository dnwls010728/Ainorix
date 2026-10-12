// OwnEngine GPU shaders (sokol-shdc annotated GLSL, Vulkan-style bindings).
//
// After editing, regenerate Shaders.glsl.h with tools/shaders/compile_shaders.sh
// (or .bat); the generated header is checked in so building the engine needs
// no shader tools. One source compiles to HLSL (D3D11), GLSL ES 3.0
// (WebGL2 / GLES3) and desktop GLSL.
//
// Conventions shared with GpuRenderer.cpp:
// - Matrices come from engine/core/Math.h: column-major, OpenGL clip space
//   (z in -1..1). Every vertex shader asks sokol-shdc to remap z for HLSL.
// - Lighting matches the software renderer (engine/render/SoftwareRenderer.cpp):
//   metallic-roughness materials (GGX), ambient + directional lights (the
//   first one casts shadows) + point lights with (1 - d/range)^2 falloff,
//   computed on non-linear colors.
// - Offscreen images are sampled with uv = gl_FragCoord / target size, which
//   gives the same orientation on every backend.

@module oe

// ----- Meshes ---------------------------------------------------------------------

// Shader graph arithmetic shared by the vertex (offset) and fragment (surface) evaluators.
// Instruction numbering and finite semantics match ShaderGraph.cpp.
@block graph_ops
// Per channel without a loop: writing value[channel] in a loop makes the D3D compiler unroll it (X3550).
float GraphFiniteChannel(float value) { return value != value ? 0.0 : clamp(value, -65504.0, 65504.0); }
vec4 GraphFinite(vec4 value) {
    return vec4(GraphFiniteChannel(value.x), GraphFiniteChannel(value.y), GraphFiniteChannel(value.z),
                GraphFiniteChannel(value.w));
}

// Instructions 8..24; `literal` is the instruction's value (swizzle channels).
vec4 GraphOp(int op, vec4 a, vec4 b, vec4 c, vec4 literal) {
    vec4 value = vec4(0.0);
    if (op == 8) value = a+b;
    else if (op == 9) value = a-b;
    else if (op == 10) value = a*b;
    else if (op == 11) {
        value = vec4(b.x == 0.0 ? 0.0 : a.x/b.x, b.y == 0.0 ? 0.0 : a.y/b.y,
                     b.z == 0.0 ? 0.0 : a.z/b.z, b.w == 0.0 ? 0.0 : a.w/b.w);
    }
    else if (op == 12) value = min(a,b);
    else if (op == 13) value = max(a,b);
    else if (op == 14) value = sin(a);
    else if (op == 15) value = cos(a);
    else if (op == 16) value = floor(a);
    else if (op == 17) value = fract(a);
    else if (op == 18) value = abs(a);
    else if (op == 19) value = min(max(a,b),c);
    else if (op == 20) value = a*(1.0-c)+b*c;
    else if (op == 21) value = step(a,b);
    else if (op == 22) value = vec4(dot(a,b));
    else if (op == 23) value = length(a) > 1e-8 ? a/length(a) : vec4(0.0);
    else if (op == 24) {
        ivec4 channels = ivec4(literal);
        value = vec4(a[channels.x],a[channels.y],a[channels.z],a[channels.w]);
    }
    return value;
}
@end

@vs mesh_vs
@hlsl_options fixup_clipspace
layout(binding=0) uniform mesh_vs_params {
    mat4 view_proj;
    mat4 model;
    mat4 normal_mat;
    vec4 item_custom; // custom attributes on shared meshes
    vec4 item_particle; // shared-mesh particle inputs; batched inputs are in particle0
};

// Skin joints (4 KB): uploaded for skinned meshes only, left over (unused) for the others.
layout(binding=4) uniform mesh_vs_skin {
    mat4 joint_palette[64];
};

// Vertex stage of a shader graph with an `offset` output (ShaderVertexOffset in ShaderGraph.cpp).
layout(binding=3) uniform mesh_vs_graph {
    vec4 vgraph_control;    // x: instructions to run (0 = no offset), y: offset output, w: simulation seconds
    vec4 vgraph_base_color; // material base color and opacity (no texture in this stage)
    vec4 vgraph_uv_rect;    // as uv_rect / uv_tiling of mesh_material
    vec4 vgraph_uv_tiling;
    vec4 vgraph_code[48];
    vec4 vgraph_value[48];
    vec4 vgraph_uniform[8];
};

in vec3 position;
in vec3 normal;
in vec2 texcoord;
in vec4 tangent;
in vec4 joint_indices;
in vec4 joint_weights;
in vec4 color0;
in vec4 particle0;
in vec4 custom0;

out vec4 v_color;
out vec4 v_particle;
out vec4 v_custom;
out vec3 v_wpos;
out vec3 v_nrm;
out vec4 v_tan;
centroid out vec2 v_uv;

@include_block graph_ops

// Inputs of the vertex stage of a graph. Compiled graphs (GraphSource.cpp) replace the body of
// VertexOffset with straight-line code that reads its inputs through these functions only.
vec4 VGraphUniform(int slot) { return vgraph_uniform[slot]; }
float VGraphTime() { return vgraph_control.w; }
vec4 VGraphBaseColor() { return vgraph_base_color * color0; }
vec4 VGraphParticle() { return particle0 + item_particle; }
vec4 VGraphCustom() { return custom0 + item_custom; }

vec3 VertexOffset(vec3 wpos, vec3 nrm, vec2 uv) {
    if (vgraph_control.x < 0.5) return vec3(0.0);
    vec4 registers[48];
    for (int i = 0; i < 48; ++i) registers[i] = vec4(0.0);
    for (int i = 0; i < 48; ++i) {
        if (i >= int(vgraph_control.x)) break;
        ivec4 code = ivec4(vgraph_code[i]);
        vec4 a = registers[code.y], b = registers[code.z], c = registers[code.w];
        vec4 value = vec4(0.0);
        if (code.x == 0) value = vgraph_value[i];
        else if (code.x == 1) value = VGraphUniform(int(vgraph_value[i].x));
        else if (code.x == 2) value = vec4(uv,0.0,0.0);
        else if (code.x == 3) value = vec4(wpos,1.0);
        else if (code.x == 4) value = vec4(nrm,0.0);
        else if (code.x == 5) value = vec4(VGraphTime());
        else if (code.x == 6) value = VGraphBaseColor();
        else if (code.x == 25) value = vec4(VGraphParticle().x);
        else if (code.x == 26) value = vec4(VGraphParticle().y);
        else if (code.x == 27) value = vec4(VGraphParticle().z);
        else if (code.x == 28) value = vec4(VGraphCustom().x);
        else if (code.x == 29) value = vec4(VGraphCustom().y);
        else if (code.x == 30) value = vec4(VGraphCustom().z);
        else if (code.x == 31) value = vec4(VGraphCustom().w);
        else if (code.x == 7) value = vec4(1.0);  // no texture in the vertex stage
        else value = GraphOp(code.x, a, b, c, vgraph_value[i]);
        registers[i] = GraphFinite(value);
    }
    return registers[int(vgraph_control.y)].xyz;
}

void main() {
    mat4 skin = mat4(1.0);
    if (dot(joint_weights, vec4(1.0)) > 0.0) {
        skin = joint_palette[int(joint_indices.x)] * joint_weights.x +
               joint_palette[int(joint_indices.y)] * joint_weights.y +
               joint_palette[int(joint_indices.z)] * joint_weights.z +
               joint_palette[int(joint_indices.w)] * joint_weights.w;
    }
    // Treat the blended xyz as a point, matching CPU TransformPoint even when float weights sum imperfectly.
    vec4 wp = model * vec4((skin * vec4(position, 1.0)).xyz, 1.0);
    mat4 skin_normal = abs(determinant(skin)) < 1e-12 ? mat4(1.0) : transpose(inverse(skin));
    v_nrm = normalize((normal_mat * skin_normal * vec4(normal, 0.0)).xyz);
    wp.xyz += VertexOffset(wp.xyz, v_nrm, (texcoord * vgraph_uv_rect.zw + vgraph_uv_rect.xy) * vgraph_uv_tiling.xy + vgraph_uv_tiling.zw);
    v_wpos = wp.xyz;
    v_tan = vec4(normalize((model * skin * vec4(tangent.xyz, 0.0)).xyz), tangent.w);
    v_uv = texcoord;
    v_color = color0;
    v_particle = particle0 + item_particle;
    v_custom = custom0 + item_custom;
    gl_Position = view_proj * wp;
}
@end

// Metallic-roughness shading. Must match Lighting::Shade and the pixel loop
// in SoftwareRenderer.cpp (GpuRendererMatchesSoftware compares them).
@block surface_material
layout(binding=1) uniform mesh_material {
    vec4 base_color;  // rgb, a: opacity
    vec4 flags;       // x: unlit, y: base texture, z: alpha cutoff (mask; 0 = off), w: double sided
    vec4 uv_rect;     // item uv rect: uv * zw + xy (sprite sheet frame, flips)
    vec4 uv_tiling;   // material: uv * xy + zw
    vec4 pbr;         // x: metallic, y: roughness, z: normal scale, w: occlusion strength
    vec4 emissive;    // rgb: emissive * intensity, w: flat shading
    vec4 maps;        // x: normal map, y: metallic-roughness map, z: emissive map, w: occlusion map
    vec4 color_range; // x: 1 for legacy RGBA8, 65504 for HDR float16 targets, y: 1 = surface_aux writes depth (depth of field),
                      // z: fog mode (0 = none, 1 = mix toward fog color, 2 = additive: fade out)
    vec4 graph_control; // node count, color output, emissive output (-1 absent), simulation seconds
    vec4 graph_outputs; // x: normal output (-1 absent)
    vec4 graph_code[48]; // opcode and three prior-register indices
    vec4 graph_value[48];
    vec4 graph_uniform[8];
};
@end

@block surface_graph
// Inputs of the fragment stage of a graph, read by compiled graphs (GraphSource.cpp) the same way.
vec4 GraphUniform(int slot) { return graph_uniform[slot]; }
float GraphTime() { return graph_control.w; }
vec4 GraphPosition() { return vec4(v_wpos, 1.0); }
vec4 GraphParticle() { return v_particle; }
vec4 GraphCustom() { return v_custom; }
// Explicit level zero matches the software texture sampler and avoids
// undefined implicit derivatives in data-dependent instruction flow.
vec4 GraphTexture(vec2 uv) { return textureLod(sampler2D(base_tex,base_smp),uv,0.0); }

// `normal_out` is the graph's lighting normal, zero when it has none.
void SurfaceGraph(vec2 uv, vec3 normal, inout vec3 base, inout float alpha, out vec3 emission, out vec3 normal_out) {
    emission = vec3(0.0);
    normal_out = vec3(0.0);
    if (graph_control.x < 0.5) return;
    vec4 registers[48];
    for (int i = 0; i < 48; ++i) registers[i] = vec4(0.0);
    for (int i = 0; i < 48; ++i) {
        if (i >= int(graph_control.x)) break;
        ivec4 code = ivec4(graph_code[i]);
        vec4 a = registers[code.y], b = registers[code.z], c = registers[code.w];
        vec4 value = vec4(0.0);
        if (code.x == 0) value = graph_value[i];
        else if (code.x == 1) value = GraphUniform(int(graph_value[i].x));
        else if (code.x == 2) value = vec4(uv,0.0,0.0);
        else if (code.x == 3) value = GraphPosition();
        else if (code.x == 4) value = vec4(normal,0.0);
        else if (code.x == 5) value = vec4(GraphTime());
        else if (code.x == 6) value = vec4(base,alpha);
        else if (code.x == 25) value = vec4(GraphParticle().x);
        else if (code.x == 26) value = vec4(GraphParticle().y);
        else if (code.x == 27) value = vec4(GraphParticle().z);
        else if (code.x == 31) value = vec4(GraphCustom().w);
        else if (code.x == 30) value = vec4(GraphCustom().z);
        else if (code.x == 29) value = vec4(GraphCustom().y);
        else if (code.x == 28) value = vec4(GraphCustom().x);
        else if (code.x == 7) value = GraphTexture(a.xy);
        else value = GraphOp(code.x, a, b, c, graph_value[i]);
        registers[i] = GraphFinite(value);
    }
    vec4 result = registers[int(graph_control.y)];
    base = max(result.rgb,vec3(0.0));
    alpha = clamp(result.a,0.0,1.0);
    if (graph_control.z >= 0.0) emission = registers[int(graph_control.z)].rgb;
    if (graph_outputs.x >= 0.0) normal_out = registers[int(graph_outputs.x)].xyz;
}

@end

@fs mesh_fs
@include_block surface_material

layout(binding=2) uniform mesh_lights {
    mat4 shadow_vp;
    vec4 ambient;
    vec4 counts;         // x: directional lights, y: point lights, z: shadows on, w: shadow strength
    vec4 shadow_params;  // x: shadow texel size in world units, y: 1 / shadow map size, z: depth bias
    vec4 eye;            // camera position
    vec4 ambient_ground; // rgb: hemisphere ambient of downward faces, w: 1 = hemisphere ambient on
    vec4 fog_color;      // rgb: fog color, w: largest fog amount (0 = off)
    vec4 fog_range;      // x: start distance, y: end distance
    vec4 view_axis;      // xyz: camera forward; frag_depth = view depth along it (depth of field)
    vec4 dir_dir[4];     // direction the light travels
    vec4 dir_color[4];
    vec4 point_pos[16];  // xyz: position, w: range
    vec4 point_color[16];
    vec4 env_sky;        // rgb: sky (zenith) color with intensity, w: 1 = sky environment present
    vec4 env_horizon;    // rgb: horizon color, w: environment reflection strength
    vec4 env_ground;     // rgb: ground (below horizon) color, w: intensity of the environment image
    vec4 env_map;        // x: 1 = the environment is an image (env_tex), y: its last (most blurred) level, z/w: cos/sin of its rotation
    vec4 env_sh[9];      // rgb: irradiance terms of the image, with intensity (EnvIrradianceSH in Environment.h)
    vec4 probe_count;    // x: reflection probes of this frame (0..4), smallest box first (ActiveProbes)
    vec4 probe_lo[4];    // xyz: box min, w: fade distance outside the box
    vec4 probe_hi[4];    // xyz: box max, w: last (most blurred) level of its picture
    vec4 probe_pos[4];   // xyz: where the picture was taken, w: intensity
    vec4 probe_opt[4];   // x: 1 = box projection, y: 1 = the picture replaces the diffuse ambient too
    vec4 probe_sh[36];   // nine irradiance terms per probe, with intensity
};

// Analytic sky color along a world direction. Mirrors SampleSky in RenderScene.h; keep identical.
vec3 sky_color(vec3 d) {
    float y = clamp(d.y, -1.0, 1.0);
    if (y >= 0.0) {
        float t = pow(y, 0.45);
        return mix(env_horizon.rgb, env_sky.rgb, t);
    }
    float t = pow(-y, 0.5);
    return mix(env_horizon.rgb, env_ground.rgb, t);
}

layout(binding=0) uniform texture2D base_tex;
layout(binding=0) uniform sampler base_smp;
layout(binding=1) uniform texture2D shadow_tex;
layout(binding=1) uniform sampler shadow_smp;
layout(binding=2) uniform texture2D normal_tex;
layout(binding=3) uniform texture2D mr_tex;
layout(binding=4) uniform texture2D emissive_tex;
layout(binding=5) uniform texture2D occlusion_tex;
layout(binding=6) uniform texture2D env_tex;  // equirectangular environment, blurrier level by level
layout(binding=2) uniform sampler env_smp;
layout(binding=7) uniform texture2D probe0_tex;  // reflection probe pictures, laid out like env_tex in world axes
layout(binding=8) uniform texture2D probe1_tex;
layout(binding=9) uniform texture2D probe2_tex;
layout(binding=10) uniform texture2D probe3_tex;

// A world direction in the environment image: turned around +Y, then to equirectangular coordinates
// (EnvMapDir in RenderScene.h, EnvUV in Environment.h).
vec3 env_dir(vec3 d) { return vec3(env_map.z * d.x + env_map.w * d.z, d.y, -env_map.w * d.x + env_map.z * d.z); }
vec2 env_uv(vec3 d) { return vec2(atan(d.x, -d.z) * 0.15915494 + 0.5, acos(clamp(d.y, -1.0, 1.0)) * 0.31830989); }

vec3 probe_texel(int i, vec2 uv, float lod) {
    if (i == 0) return textureLod(sampler2D(probe0_tex, env_smp), uv, lod).rgb;
    if (i == 1) return textureLod(sampler2D(probe1_tex, env_smp), uv, lod).rgb;
    if (i == 2) return textureLod(sampler2D(probe2_tex, env_smp), uv, lod).rgb;
    return textureLod(sampler2D(probe3_tex, env_smp), uv, lod).rgb;
}

in vec4 v_color;
in vec4 v_particle;
in vec4 v_custom;
in vec3 v_wpos;
in vec3 v_nrm;
in vec4 v_tan;
centroid in vec2 v_uv;

layout(location=0) out vec4 frag_color;
// View depth for depth of field when the pass has a second target (blended pipelines mask it off).
layout(location=1) out vec4 frag_depth;

const float PI = 3.14159265;

// 1 = fully lit, 0 = fully shadowed (3x3 PCF with hardware compare).
float shadow_lit(vec3 wpos, vec3 n) {
    vec4 c = shadow_vp * vec4(wpos + n * (shadow_params.x * 1.5), 1.0);
    vec3 p = c.xyz / c.w;
    vec2 uv;
    uv.x = p.x * 0.5 + 0.5;
#if SOKOL_GLSL
    uv.y = p.y * 0.5 + 0.5;  // GL render targets start at the bottom row
#else
    uv.y = 0.5 - p.y * 0.5;
#endif
    float z = p.z * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 || z > 1.0) {
        return 1.0;
    }
    float lit = 0.0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            vec2 o = vec2(float(dx), float(dy)) * shadow_params.y;
            lit += texture(sampler2DShadow(shadow_tex, shadow_smp), vec3(uv + o, z - shadow_params.z));
        }
    }
    return 1.0 - counts.w * (1.0 - lit / 9.0);
}

vec3 brdf(vec3 n, vec3 v, vec3 l, vec3 diffuse, vec3 f0, float a2, float k, float ndv, float gv) {
    float ndl = dot(n, l);
    if (ndl <= 0.0) {
        return vec3(0.0);
    }
    vec3 h = normalize(l + v);
    float ndh = max(dot(n, h), 0.0);
    float vdh = max(dot(v, h), 0.0);
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    float D = a2 / (PI * d * d);
    float gl = ndl / (ndl * (1.0 - k) + k);
    float fw = pow(1.0 - vdh, 5.0);
    vec3 F = f0 * (1.0 - fw) + vec3(fw);
    float spec = D * gv * gl / (4.0 * ndv * ndl + 1e-4) * PI;
    return (diffuse + F * spec) * ndl;
}

@include_block graph_ops
@include_block surface_graph

void main() {
    vec2 uv = (v_uv * uv_rect.zw + uv_rect.xy) * uv_tiling.xy + uv_tiling.zw;
    vec3 base = base_color.rgb * v_color.rgb;
    float alpha = base_color.a * v_color.a;
    if (flags.y > 0.5) {
        vec4 texel = texture(sampler2D(base_tex, base_smp), uv);
        base *= texel.rgb;
        alpha *= texel.a;
    }
    vec3 graphNormal = normalize(v_nrm);
    if (graph_control.x > 0.5 && emissive.w > 0.5) {
        vec3 face = normalize(cross(dFdx(v_wpos),dFdy(v_wpos)));
        graphNormal = dot(face,graphNormal) < 0.0 ? -face : face;
    }
    if (flags.w > 0.5 && !gl_FrontFacing) graphNormal = -graphNormal;
    vec3 graphEmission;
    vec3 graphLightNormal;
    SurfaceGraph(uv,graphNormal,base,alpha,graphEmission,graphLightNormal);
    if (alpha < flags.z) {
        discard;
    }
    vec3 color = base;
    if (flags.x < 0.5) {
        vec3 ng = normalize(v_nrm);
        if (emissive.w > 0.5) {
            vec3 face = normalize(cross(dFdx(v_wpos), dFdy(v_wpos)));
            ng = dot(face, ng) < 0.0 ? -face : face;
        }
        if (length(graphLightNormal) > 1e-8) {
            ng = normalize(graphLightNormal);
        }
        if (flags.w > 0.5 && !gl_FrontFacing) {
            ng = -ng;
        }
        vec3 n = ng;
        if (maps.x > 0.5) {
            vec3 t = v_tan.xyz - ng * dot(ng, v_tan.xyz);
            if (length(t) > 1e-6) {
                t = normalize(t);
                vec3 b = cross(ng, t) * (v_tan.w < 0.0 ? -1.0 : 1.0);
                vec3 nt = texture(sampler2D(normal_tex, base_smp), uv).rgb * 2.0 - 1.0;
                n = normalize(t * (nt.x * pbr.z) + b * (nt.y * pbr.z) + ng * nt.z);
            }
        }
        float metallic = pbr.x;
        float roughness = pbr.y;
        if (maps.y > 0.5) {
            vec3 mr = texture(sampler2D(mr_tex, base_smp), uv).rgb;
            roughness *= mr.g;
            metallic *= mr.b;
        }
        float ao = 1.0;
        if (maps.w > 0.5) {
            ao = 1.0 + pbr.w * (texture(sampler2D(occlusion_tex, base_smp), uv).r - 1.0);
        }
        vec3 v = normalize(eye.xyz - v_wpos);
        float ndv = max(dot(n, v), 1e-4);
        vec3 diffuse = base * (1.0 - metallic);
        vec3 f0 = vec3(0.04) * (1.0 - metallic) + base * metallic;
        float r = clamp(roughness, 0.04, 1.0);
        float a2 = r * r * r * r;
        float k = (r + 1.0) * (r + 1.0) / 8.0;
        float gv = ndv / (ndv * (1.0 - k) + k);
        // Ambient: diffuse plus reflected surroundings, approximated by a
        // hemisphere that is brighter above (sky) than below (ground).
        vec3 refl = n * (2.0 * dot(n, v)) - v;
        float sky = 0.6 + 1.6 * (refl.y * 0.5 + 0.5);
        float fe = pow(1.0 - ndv, 5.0);
        vec3 fenv = f0 + (max(vec3(1.0 - r), f0) - f0) * fe;
        vec3 dBase;  // ambient light: diffuse, and reflected (sBase)
        vec3 sBase;
        if (env_sky.w > 0.5) {
            // Image-based lighting: diffuse ambient from the whole sky, specular from the sky along
            // the reflection vector, blurrier as the surface roughens. Mirrors EnvDiffuse / EnvSpecular
            // in RenderScene.h (the software renderer).
            vec3 ambDiffuse;
            vec3 envSpec;
            if (env_map.x > 0.5) {
                vec3 m = env_dir(n);
                ambDiffuse = max(env_sh[0].rgb + env_sh[1].rgb * m.y + env_sh[2].rgb * m.z + env_sh[3].rgb * m.x +
                                 env_sh[4].rgb * (m.x * m.y) + env_sh[5].rgb * (m.y * m.z) +
                                 env_sh[6].rgb * (3.0 * m.z * m.z - 1.0) + env_sh[7].rgb * (m.x * m.z) +
                                 env_sh[8].rgb * (m.x * m.x - m.y * m.y), vec3(0.0));
                float lod = (r - 0.04) / 0.96 * env_map.y;
                envSpec = textureLod(sampler2D(env_tex, env_smp), env_uv(env_dir(refl)), lod).rgb * env_ground.w;
            } else {
                float up = n.y * 0.5 + 0.5;
                ambDiffuse = mix(sky_color(vec3(0.0, -1.0, 0.0)), sky_color(vec3(0.0, 1.0, 0.0)), up);
                vec3 dull = (sky_color(vec3(0.0, 1.0, 0.0)) + sky_color(vec3(0.0, -1.0, 0.0))) * 0.5;
                envSpec = mix(sky_color(refl), dull, r);
            }
            dBase = ambDiffuse * diffuse;
            sBase = fenv * envSpec * env_horizon.w;
        } else {
            vec3 amb = ambient.rgb;
            if (ambient_ground.w > 0.5) {
                float up = n.y * 0.5 + 0.5;
                amb = ambient_ground.rgb * (1.0 - up) + ambient.rgb * up;
            }
            dBase = amb * diffuse;
            sBase = amb * fenv * (sky * (1.0 - 0.75 * r));
        }
        // Reflection probes take over the ambient light where their boxes are: each one its share of
        // what the probes before it left. Mirrors the probe loop of Lighting::Shade (ProbeWeight,
        // ProbeDir, ProbeSpecular, ProbeDiffuse in RenderScene.h).
        float remaining = 1.0;
        vec3 pd = vec3(0.0);
        vec3 ps = vec3(0.0);
        int probes = int(probe_count.x);
        for (int i = 0; i < 4; i++) {
            if (i >= probes) {
                break;
            }
            vec3 over = max(probe_lo[i].xyz - v_wpos, v_wpos - probe_hi[i].xyz);
            float w = (1.0 - clamp(max(over.x, max(over.y, over.z)) / probe_lo[i].w, 0.0, 1.0)) * remaining;
            if (w <= 0.0) {
                continue;
            }
            vec3 dir = refl;
            if (probe_opt[i].x > 0.5) {
                vec3 rd = vec3(abs(refl.x) < 1e-6 ? 1e-6 : refl.x, abs(refl.y) < 1e-6 ? 1e-6 : refl.y, abs(refl.z) < 1e-6 ? 1e-6 : refl.z);
                vec3 tf = max((probe_hi[i].xyz - v_wpos) / rd, (probe_lo[i].xyz - v_wpos) / rd);
                float t = max(min(tf.x, min(tf.y, tf.z)), 0.0);
                vec3 hit = v_wpos + refl * t - probe_pos[i].xyz;
                if (dot(hit, hit) > 1e-12) {
                    dir = normalize(hit);
                }
            }
            vec3 dif = dBase;
            if (probe_opt[i].y > 0.5) {
                int b = i * 9;
                dif = max(probe_sh[b].rgb + probe_sh[b + 1].rgb * n.y + probe_sh[b + 2].rgb * n.z + probe_sh[b + 3].rgb * n.x +
                          probe_sh[b + 4].rgb * (n.x * n.y) + probe_sh[b + 5].rgb * (n.y * n.z) +
                          probe_sh[b + 6].rgb * (3.0 * n.z * n.z - 1.0) + probe_sh[b + 7].rgb * (n.x * n.z) +
                          probe_sh[b + 8].rgb * (n.x * n.x - n.y * n.y), vec3(0.0)) * diffuse;
            }
            pd += dif * w;
            ps += fenv * probe_texel(i, env_uv(dir), (r - 0.04) / 0.96 * probe_hi[i].w) * probe_pos[i].w * w;
            remaining -= w;
        }
        color = (dBase * remaining + pd + sBase * remaining + ps) * ao;
        int dirs = int(counts.x);
        // The shadow lookup stays outside the light loop: a compare sample inside a loop with a
        // varying count makes the D3D compiler unroll it (X3570) and slows every compile.
        float sunLit = (dirs > 0 && counts.z > 0.5) ? shadow_lit(v_wpos, ng) : 1.0;
        for (int i = 0; i < 4; i++) {
            if (i >= dirs) {
                break;
            }
            float lit = i == 0 ? sunLit : 1.0;
            if (lit > 0.0) {
                color += brdf(n, v, -dir_dir[i].xyz, diffuse, f0, a2, k, ndv, gv) * dir_color[i].rgb * lit;
            }
        }
        int points = int(counts.y);
        for (int i = 0; i < 16; i++) {
            if (i >= points) {
                break;
            }
            vec3 d = point_pos[i].xyz - v_wpos;
            float dist = length(d);
            float range = point_pos[i].w;
            if (dist >= range || dist < 1e-5) {
                continue;
            }
            float fall = 1.0 - dist / range;
            color += brdf(n, v, d / dist, diffuse, f0, a2, k, ndv, gv) * point_color[i].rgb * (fall * fall);
        }
    }
    vec3 em = emissive.rgb;
    if (maps.z > 0.5) {
        em *= texture(sampler2D(emissive_tex, base_smp), uv).rgb;
    }
    vec3 outc = color + em + graphEmission;
    if (color_range.z > 0.5 && fog_color.w > 0.0) {  // FogAmount in PostProcess.h
        float f = fog_color.w * clamp((length(eye.xyz - v_wpos) - fog_range.x) / (fog_range.y - fog_range.x), 0.0, 1.0);
        outc = color_range.z > 1.5 ? outc * (1.0 - f) : outc * (1.0 - f) + fog_color.rgb * f;
    }
    frag_color = vec4(clamp(outc, 0.0, color_range.x), clamp(alpha, 0.0, 1.0));
    frag_depth = vec4(dot(v_wpos - eye.xyz, view_axis.xyz), 0.0, 0.0, 1.0);
}
@end

@program mesh mesh_vs mesh_fs

// ----- Position-only passes: shadow map depth and the selection mask -------------

@vs pos_vs
@hlsl_options fixup_clipspace
layout(binding=0) uniform pos_vs_params {
    mat4 mvp;
};

layout(binding=2) uniform pos_vs_skin {
    mat4 joint_palette[64];
};

in vec3 position;
in vec4 joint_indices;
in vec4 joint_weights;

void main() {
    mat4 skin = mat4(1.0);
    if (dot(joint_weights, vec4(1.0)) > 0.0) {
        skin = joint_palette[int(joint_indices.x)] * joint_weights.x +
               joint_palette[int(joint_indices.y)] * joint_weights.y +
               joint_palette[int(joint_indices.z)] * joint_weights.z +
               joint_palette[int(joint_indices.w)] * joint_weights.w;
    }
    gl_Position = mvp * vec4((skin * vec4(position, 1.0)).xyz, 1.0);
}
@end

@fs depth_fs
void main() {
}
@end

@fs solid_fs
layout(binding=1) uniform solid_params {
    vec4 color;
};

out vec4 frag_color;

void main() {
    frag_color = color;
}
@end

// Depth buffer value (NDC z * 0.5 + 0.5, like the software renderer's depth) as a color, for depth of field.
@fs depth_color_fs
out vec4 frag_color;

void main() {
    frag_color = vec4(gl_FragCoord.z, 0.0, 0.0, 1.0);
}
@end

@program shadow pos_vs depth_fs
@program solid pos_vs solid_fs
@program scene_depth pos_vs depth_color_fs

@fs surface_aux_fs
@include_block surface_material
layout(binding=0) uniform texture2D base_tex;
layout(binding=0) uniform sampler base_smp;
in vec4 v_color;
in vec4 v_particle;
in vec4 v_custom;
in vec3 v_wpos;
in vec3 v_nrm;
in vec4 v_tan;
in vec2 v_uv;
out vec4 frag_color;
@include_block graph_ops
@include_block surface_graph
void main() {
    vec2 uv = (v_uv * uv_rect.zw + uv_rect.xy) * uv_tiling.xy + uv_tiling.zw;
    vec3 base = base_color.rgb * v_color.rgb;
    float alpha = base_color.a * v_color.a;
    if (flags.y > 0.5) {
        vec4 texel = textureLod(sampler2D(base_tex,base_smp),uv,0.0);
        base *= texel.rgb;
        alpha *= texel.a;
    }
    vec3 normal = normalize(v_nrm);
    if (emissive.w > 0.5) {
        vec3 face = normalize(cross(dFdx(v_wpos),dFdy(v_wpos)));
        normal = dot(face,normal) < 0.0 ? -face : face;
    }
    if (!gl_FrontFacing) normal = -normal;
    vec3 emission;
    vec3 lightNormal;
    SurfaceGraph(uv,normal,base,alpha,emission,lightNormal);
    if (alpha < flags.z) discard;
    frag_color = color_range.y > 0.5 ? vec4(gl_FragCoord.z, 0.0, 0.0, 1.0) : vec4(1.0);
}
@end
@program surface_aux mesh_vs surface_aux_fs

// ----- Vertex-colored lines (grid, colliders, debug.draw) and screen-space UI -----

@vs line_vs
@hlsl_options fixup_clipspace
layout(binding=0) uniform line_params {
    mat4 view_proj;
};

in vec3 position;
in vec4 color0;

out vec4 v_color;

void main() {
    gl_Position = view_proj * vec4(position, 1.0);
    // Same tolerance as the software renderer: lines lying on a surface win.
    gl_Position.z -= 2.0e-4 * gl_Position.w;
    v_color = color0;
}
@end

@vs ui_vs
@hlsl_options fixup_clipspace
layout(binding=0) uniform ui_params {
    vec4 screen;  // xy: 2 / target size in pixels
};

in vec2 position;     // pixels, top-left origin
in vec2 texcoord0;    // normalized texture coordinates
in vec4 color0;       // color * alpha
in vec4 color1;       // border color and alpha
in vec4 shape_rect;   // rounded rectangle x0, y0, x1, y1 (pixels)
in vec4 shape_params; // radius, border width, textured, shaped

out vec2 v_pix;
out vec2 v_uv;
out vec4 v_color;
out vec4 v_border;
out vec4 v_rect;
out vec4 v_params;

void main() {
    gl_Position = vec4(position.x * screen.x - 1.0, 1.0 - position.y * screen.y, 0.0, 1.0);
    v_pix = position;
    v_uv = texcoord0;
    v_color = color0;
    v_border = color1;
    v_rect = shape_rect;
    v_params = shape_params;
}
@end

// Must match ShadeUIQuad in render/UI.cpp (the software renderer).
@fs ui_fs
layout(binding=0) uniform texture2D ui_tex;
layout(binding=0) uniform sampler ui_smp;

in vec2 v_pix;
in vec2 v_uv;
in vec4 v_color;
in vec4 v_border;
in vec4 v_rect;
in vec4 v_params;

out vec4 frag_color;

void main() {
    vec4 c = v_color;
    if (v_params.z > 0.5) {
        c *= texture(sampler2D(ui_tex, ui_smp), v_uv);
    }
    if (v_params.w > 0.5) {
        vec2 center = (v_rect.xy + v_rect.zw) * 0.5;
        vec2 half_size = (v_rect.zw - v_rect.xy) * 0.5;
        float r = min(v_params.x, min(half_size.x, half_size.y));
        vec2 q = abs(v_pix - center) - half_size + r;
        float d = length(max(q, vec2(0.0))) + min(max(q.x, q.y), 0.0) - r;
        float cover = clamp(0.5 - d, 0.0, 1.0);
        if (v_params.y > 0.0) {
            float inner = clamp(0.5 - (d + v_params.y), 0.0, 1.0);
            c = mix(v_border, c, inner);
        }
        c.a *= cover;
    }
    frag_color = c;
}
@end

@fs color_fs
in vec4 v_color;
out vec4 frag_color;

void main() {
    frag_color = v_color;
}
@end

@program line line_vs color_fs
@program ui ui_vs ui_fs

// ----- Composite: scene image -> output, plus the editor selection outline -------
// Screen-space effects (tone mapping, bloom, color grading...) belong here or
// in extra passes between the scene pass and this one.

@vs fsq_vs
@hlsl_options fixup_clipspace
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
@end

@fs composite_fs
layout(binding=0) uniform composite_params {
    vec4 target;         // xy: output size in pixels
    vec4 outline_color;  // rgb; a > 0 enables the outline
    vec4 vignette;       // x: strength, y: radius, z: softness; zero strength leaves color unchanged
    vec4 tone_mapping;   // x: exposure multiplier, y: 1 = Reinhard, 0 = disabled
    vec4 bloom_strength; // x: blurred HDR highlight strength, before exposure
    vec4 post_stage;     // x: source already processed, y: FXAA enabled, zw: reciprocal source size
    vec4 grade_shadow;   // rgb: shadow tint, w: 1 = color grading on
    vec4 grade_high;     // rgb: highlight tint
    vec4 grade_params;   // x: saturation, y: contrast
};

layout(binding=0) uniform texture2D scene_tex;
layout(binding=0) uniform sampler scene_smp;
layout(binding=1) uniform texture2D mask_tex;
layout(binding=1) uniform sampler mask_smp;
layout(binding=2) uniform texture2D bloom_tex;
layout(binding=2) uniform sampler bloom_smp;

out vec4 frag_color;

vec3 DisplayColor(vec2 uv) {
    vec3 c = texture(sampler2D(scene_tex, scene_smp), uv).rgb;
    if (post_stage.x > 0.5) return c;
    if (bloom_strength.x > 0.0) {
        c += texture(sampler2D(bloom_tex, bloom_smp), uv).rgb * bloom_strength.x;
        c = clamp(c, 0.0, 65504.0);
    }
    c *= tone_mapping.x;
    if (tone_mapping.y > 1.5) c = c * (2.51 * c + 0.03) / (c * (2.43 * c + 0.59) + 0.14);
    else if (tone_mapping.y > 0.5) c = c / (vec3(1.0) + c);
    c = clamp(c, 0.0, 1.0);
    if (grade_shadow.w > 0.5) {  // Grade in PostProcess.h
        const vec3 lw = vec3(0.2126, 0.7152, 0.0722);
        float t = clamp(dot(c, lw) / 0.9, 0.0, 1.0);
        t = t * t * (3.0 - 2.0 * t);
        c *= grade_shadow.rgb * (1.0 - t) + grade_high.rgb * t;
        float l = dot(c, lw);
        c = vec3(l) + (c - vec3(l)) * grade_params.x;
        c = (c - 0.5) * grade_params.y + 0.5;
        c = clamp(c, 0.0, 1.0);
    }
    if (vignette.x > 0.0) {
        float t = clamp((length(uv * 2.0 - 1.0) - vignette.y) / vignette.z, 0.0, 1.0);
        c *= 1.0 - vignette.x * t * t * (3.0 - 2.0 * t);
    }
    return c;
}

float FxaaLuma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

vec3 FxaaColor(vec2 uv) {
    vec2 step_uv = post_stage.zw;
    vec3 center = DisplayColor(uv);
    float m = FxaaLuma(center);
    float nw = FxaaLuma(DisplayColor(uv + vec2(-1.0,-1.0) * step_uv));
    float ne = FxaaLuma(DisplayColor(uv + vec2( 1.0,-1.0) * step_uv));
    float sw = FxaaLuma(DisplayColor(uv + vec2(-1.0, 1.0) * step_uv));
    float se = FxaaLuma(DisplayColor(uv + vec2( 1.0, 1.0) * step_uv));
    float lo = min(m, min(min(nw,ne), min(sw,se)));
    float hi = max(m, max(max(nw,ne), max(sw,se)));
    if (hi - lo < max(1.0 / 32.0, hi * 0.125)) return center;
    vec2 dir = vec2(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));
    float reduce = max((nw + ne + sw + se) * (0.25 * 0.125), 1.0 / 128.0);
    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), vec2(-8.0), vec2(8.0)) * step_uv;
    vec3 a = (DisplayColor(uv - dir / 6.0) + DisplayColor(uv + dir / 6.0)) * 0.5;
    vec3 b = a * 0.5 + (DisplayColor(uv - dir * 0.5) + DisplayColor(uv + dir * 0.5)) * 0.25;
    float lb = FxaaLuma(b);
    return lb < lo || lb > hi ? a : b;
}

void main() {
    vec2 uv = gl_FragCoord.xy / target.xy;
    vec3 c = post_stage.y > 0.5 ? FxaaColor(uv) : DisplayColor(uv);
    if (outline_color.a > 0.0) {
        ivec2 size = textureSize(sampler2D(mask_tex, mask_smp), 0);
        ivec2 p = ivec2(uv * vec2(size));
        if (texelFetch(sampler2D(mask_tex, mask_smp), p, 0).r < 0.5) {
            bool edge = false;
            for (int dy = -2; dy <= 2; dy++) {
                for (int dx = -2; dx <= 2; dx++) {
                    ivec2 q = clamp(p + ivec2(dx, dy), ivec2(0, 0), size - ivec2(1, 1));
                    edge = edge || texelFetch(sampler2D(mask_tex, mask_smp), q, 0).r > 0.5;
                }
            }
            if (edge) {
                c = outline_color.rgb;
            }
        }
    }
    frag_color = vec4(c, 1.0);
}
@end

@program composite fsq_vs composite_fs

// Full-resolution separable tent filter. Must match SoftwareRenderer's bloom
// passes: brightest-channel extraction preserves hue, integer weights and
// clamped texel coordinates preserve constant highlights at screen borders.
@fs bloom_fs
layout(binding=0) uniform bloom_params {
    vec4 bloom_filter; // x: radius (1..32), y: threshold, z: 0 horizontal/extract, 1 vertical
};
layout(binding=0) uniform texture2D bloom_source_tex;
layout(binding=0) uniform sampler bloom_source_smp;
out vec4 frag_color;

void main() {
    int radius = int(bloom_filter.x);
    ivec2 size = textureSize(sampler2D(bloom_source_tex, bloom_source_smp), 0);
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 sum = vec3(0.0);
    for (int delta = -32; delta <= 32; ++delta) {
        if (abs(delta) <= radius) {
            ivec2 q = p + (bloom_filter.z < 0.5 ? ivec2(delta, 0) : ivec2(0, delta));
            q = clamp(q, ivec2(0), size - ivec2(1));
            vec3 c = texelFetch(sampler2D(bloom_source_tex, bloom_source_smp), q, 0).rgb;
            if (bloom_filter.z < 0.5) {
                float peak = max(max(c.r, c.g), c.b);
                c = peak > bloom_filter.y ? c * ((peak - bloom_filter.y) / peak) : vec3(0.0);
            }
            sum += c * float(radius + 1 - abs(delta));
        }
    }
    frag_color = vec4(sum / float((radius + 1) * (radius + 1)), 1.0);
}
@end

@program bloom fsq_vs bloom_fs

// Depth of field: separable gather over the scene color, weighted by each sample's blur radius.
// Must match DofViewDepth/DofCoc/DofWeight in PostProcess.h and the software renderer's passes.
@fs dof_fs
layout(binding=0) uniform dof_params {
    vec4 dof_lens; // x: largest radius in pixels (1..16), y: focus depth, z: sharp range, w: falloff
    vec4 dof_proj; // x: projection (2,2), y: projection (2,3), z: 1 = orthographic, w: 0 horizontal, 1 vertical
    vec4 dof_mode; // x: 1 = the depth texture holds view depth (scene pass MRT), 0 = depth buffer values
};
layout(binding=0) uniform texture2D dof_source_tex;
layout(binding=0) uniform sampler dof_source_smp;
layout(binding=1) uniform texture2D dof_depth_tex;
layout(binding=1) uniform sampler dof_depth_smp;
out vec4 frag_color;

float DofDepth(ivec2 q) {
    float stored = texelFetch(sampler2D(dof_depth_tex, dof_depth_smp), q, 0).r;
    if (dof_mode.x > 0.5) return stored;
    float ndc = stored * 2.0 - 1.0;
    return dof_proj.z > 0.5 ? (dof_proj.y - ndc) / dof_proj.x : dof_proj.y / (ndc + dof_proj.x);
}

float DofCoc(float depth) {
    return dof_lens.x * clamp((abs(depth - dof_lens.y) - dof_lens.z) / dof_lens.w, 0.0, 1.0);
}

void main() {
    int radius = int(dof_lens.x);
    ivec2 size = textureSize(sampler2D(dof_source_tex, dof_source_smp), 0);
    ivec2 p = ivec2(gl_FragCoord.xy);
    float center_depth = DofDepth(p);
    float center_coc = DofCoc(center_depth);
    vec3 sum = vec3(0.0);
    float total = 0.0;
    for (int delta = -16; delta <= 16; ++delta) {
        if (abs(delta) <= radius) {
            ivec2 q = p + (dof_proj.w < 0.5 ? ivec2(delta, 0) : ivec2(0, delta));
            q = clamp(q, ivec2(0), size - ivec2(1));
            float depth = DofDepth(q);
            float coc = DofCoc(depth);
            if (depth > center_depth) coc = min(coc, center_coc);
            float weight = clamp(coc - float(abs(delta)) + 1.0, 0.0, 1.0);
            sum += texelFetch(sampler2D(dof_source_tex, dof_source_smp), q, 0).rgb * weight;
            total += weight;
        }
    }
    frag_color = vec4(sum / total, 1.0);
}
@end

@program dof fsq_vs dof_fs

// ----- Skybox background -----------------------------------------------------------
// A fullscreen triangle drawn first in the scene pass: the sky along each pixel's world ray - the
// environment image, or the analytic gradient plus a soft sun disc. Mirrors SampleSkyBackground in
// RenderScene.h.
@vs sky_vs
@hlsl_options fixup_clipspace
out vec2 v_ndc;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_ndc = p * 2.0 - 1.0;
    gl_Position = vec4(v_ndc, 1.0, 1.0);
}
@end

@fs sky_fs
layout(binding=0) uniform sky_params {
    mat4 inv_view_proj;  // (proj * view)^-1, engine (OpenGL) convention
    vec4 sky_c;          // rgb: zenith color
    vec4 horizon_c;      // rgb: horizon color
    vec4 ground_c;       // rgb: ground color
    vec4 sun_p;          // xyz: toward the sun (-light direction), w: cosine of the disc radius
    vec4 sun_c;          // rgb: sun color, w: 1 = draw the sun
    vec4 sky_map;        // x: 1 = the sky is an image (sky_tex), y: its intensity, z/w: cos/sin of its rotation
};
layout(binding=0) uniform texture2D sky_tex;
layout(binding=0) uniform sampler sky_smp;
in vec2 v_ndc;
out vec4 frag_color;
out vec4 frag_depth;   // scene pass MRT (depth of field): background is far
vec3 skyc(vec3 d) {
    float y = clamp(d.y, -1.0, 1.0);
    if (y >= 0.0) return mix(horizon_c.rgb, sky_c.rgb, pow(y, 0.45));
    return mix(horizon_c.rgb, ground_c.rgb, pow(-y, 0.5));
}
void main() {
    vec4 a = inv_view_proj * vec4(v_ndc, -1.0, 1.0);
    vec4 b = inv_view_proj * vec4(v_ndc, 1.0, 1.0);
    vec3 dir = normalize(b.xyz / b.w - a.xyz / a.w);
    vec3 c;
    if (sky_map.x > 0.5) {
        vec3 m = vec3(sky_map.z * dir.x + sky_map.w * dir.z, dir.y, -sky_map.w * dir.x + sky_map.z * dir.z);
        vec2 uv = vec2(atan(m.x, -m.z) * 0.15915494 + 0.5, acos(clamp(m.y, -1.0, 1.0)) * 0.31830989);
        c = textureLod(sampler2D(sky_tex, sky_smp), uv, 0.0).rgb * sky_map.y;
    } else {
        c = skyc(dir);
        if (sun_c.w > 0.5) {
            float d = dot(dir, sun_p.xyz);
            float disc = clamp((d - sun_p.w) / max(1e-5, 1.0 - sun_p.w), 0.0, 1.0);
            c += sun_c.rgb * (disc * disc);
        }
    }
    frag_color = vec4(c, 1.0);
    frag_depth = vec4(1.0e6, 0.0, 0.0, 1.0);
}
@end

@program sky sky_vs sky_fs
