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

@vs mesh_vs
@hlsl_options fixup_clipspace
layout(binding=0) uniform mesh_vs_params {
    mat4 view_proj;
    mat4 model;
    mat4 normal_mat;
    mat4 joint_palette[64];
};

in vec3 position;
in vec3 normal;
in vec2 texcoord;
in vec4 tangent;
in vec4 joint_indices;
in vec4 joint_weights;

out vec3 v_wpos;
out vec3 v_nrm;
out vec4 v_tan;
centroid out vec2 v_uv;

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
    v_wpos = wp.xyz;
    mat4 skin_normal = abs(determinant(skin)) < 1e-12 ? mat4(1.0) : transpose(inverse(skin));
    v_nrm = normalize((normal_mat * skin_normal * vec4(normal, 0.0)).xyz);
    v_tan = vec4(normalize((model * skin * vec4(tangent.xyz, 0.0)).xyz), tangent.w);
    v_uv = texcoord;
    gl_Position = view_proj * wp;
}
@end

// Metallic-roughness shading. Must match Lighting::Shade and the pixel loop
// in SoftwareRenderer.cpp (GpuRendererMatchesSoftware compares them).
@fs mesh_fs
layout(binding=1) uniform mesh_material {
    vec4 base_color;  // rgb, a: opacity
    vec4 flags;       // x: unlit, y: base texture, z: alpha cutoff (mask; 0 = off), w: double sided
    vec4 uv_rect;     // item uv rect: uv * zw + xy (sprite sheet frame, flips)
    vec4 uv_tiling;   // material: uv * xy + zw
    vec4 pbr;         // x: metallic, y: roughness, z: normal scale, w: occlusion strength
    vec4 emissive;    // rgb: emissive * intensity, w: flat shading
    vec4 maps;        // x: normal map, y: metallic-roughness map, z: emissive map, w: occlusion map
    vec4 color_range; // x: 1 for legacy RGBA8, 65504 for HDR float16 targets
};

layout(binding=2) uniform mesh_lights {
    mat4 shadow_vp;
    vec4 ambient;
    vec4 counts;         // x: directional lights, y: point lights, z: shadows on, w: shadow strength
    vec4 shadow_params;  // x: shadow texel size in world units, y: 1 / shadow map size, z: depth bias
    vec4 eye;            // camera position
    vec4 dir_dir[4];     // direction the light travels
    vec4 dir_color[4];
    vec4 point_pos[16];  // xyz: position, w: range
    vec4 point_color[16];
};

layout(binding=0) uniform texture2D base_tex;
layout(binding=0) uniform sampler base_smp;
layout(binding=1) uniform texture2D shadow_tex;
layout(binding=1) uniform sampler shadow_smp;
layout(binding=2) uniform texture2D normal_tex;
layout(binding=3) uniform texture2D mr_tex;
layout(binding=4) uniform texture2D emissive_tex;
layout(binding=5) uniform texture2D occlusion_tex;

in vec3 v_wpos;
in vec3 v_nrm;
in vec4 v_tan;
centroid in vec2 v_uv;

out vec4 frag_color;

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

void main() {
    vec2 uv = (v_uv * uv_rect.zw + uv_rect.xy) * uv_tiling.xy + uv_tiling.zw;
    vec3 base = base_color.rgb;
    float alpha = base_color.a;
    if (flags.y > 0.5) {
        vec4 texel = texture(sampler2D(base_tex, base_smp), uv);
        base *= texel.rgb;
        alpha *= texel.a;
    }
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
        color = ambient.rgb * (diffuse + fenv * (sky * (1.0 - 0.75 * r))) * ao;
        int dirs = int(counts.x);
        for (int i = 0; i < 4; i++) {
            if (i >= dirs) {
                break;
            }
            float lit = (i == 0 && counts.z > 0.5) ? shadow_lit(v_wpos, ng) : 1.0;
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
    frag_color = vec4(clamp(color + em, 0.0, color_range.x), clamp(alpha, 0.0, 1.0));
}
@end

@program mesh mesh_vs mesh_fs

// ----- Position-only passes: shadow map depth and the selection mask -------------

@vs pos_vs
@hlsl_options fixup_clipspace
layout(binding=0) uniform pos_vs_params {
    mat4 mvp;
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

@program shadow pos_vs depth_fs
@program solid pos_vs solid_fs

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
};

layout(binding=0) uniform texture2D scene_tex;
layout(binding=0) uniform sampler scene_smp;
layout(binding=1) uniform texture2D mask_tex;
layout(binding=1) uniform sampler mask_smp;
layout(binding=2) uniform texture2D bloom_tex;
layout(binding=2) uniform sampler bloom_smp;

out vec4 frag_color;

void main() {
    vec2 uv = gl_FragCoord.xy / target.xy;
    vec3 c = texture(sampler2D(scene_tex, scene_smp), uv).rgb;
    if (bloom_strength.x > 0.0) {
        c += texture(sampler2D(bloom_tex, bloom_smp), uv).rgb * bloom_strength.x;
        c = clamp(c, 0.0, 65504.0);
    }
    c *= tone_mapping.x;
    if (tone_mapping.y > 0.5) c = c / (vec3(1.0) + c);
    c = clamp(c, 0.0, 1.0);
    if (vignette.x > 0.0) {
        float t = clamp((length(uv * 2.0 - 1.0) - vignette.y) / vignette.z, 0.0, 1.0);
        c *= 1.0 - vignette.x * t * t * (3.0 - 2.0 * t);
    }
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
