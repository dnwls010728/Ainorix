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
//   ambient + directional lights (the first one casts shadows) + point lights
//   with (1 - d/range)^2 falloff, computed on non-linear colors.
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
};

in vec3 position;
in vec3 normal;
in vec2 texcoord;

out vec3 v_wpos;
out vec3 v_nrm;
centroid out vec2 v_uv;

void main() {
    vec4 wp = model * vec4(position, 1.0);
    v_wpos = wp.xyz;
    v_nrm = (normal_mat * vec4(normal, 0.0)).xyz;
    v_uv = texcoord;
    gl_Position = view_proj * wp;
}
@end

@fs mesh_fs
layout(binding=1) uniform mesh_material {
    vec4 base_color;
    vec4 flags;    // x: unlit, y: textured, z: alpha cutoff (sprites; 0 = off)
    vec4 uv_rect;  // uv = v_uv * zw + xy (sprite sheet frame, flips)
};

layout(binding=2) uniform mesh_lights {
    mat4 shadow_vp;
    vec4 ambient;
    vec4 counts;         // x: directional lights, y: point lights, z: shadows on, w: shadow strength
    vec4 shadow_params;  // x: shadow texel size in world units, y: 1 / shadow map size, z: depth bias
    vec4 dir_dir[4];     // direction the light travels
    vec4 dir_color[4];
    vec4 point_pos[16];  // xyz: position, w: range
    vec4 point_color[16];
};

layout(binding=0) uniform texture2D base_tex;
layout(binding=0) uniform sampler base_smp;
layout(binding=1) uniform texture2D shadow_tex;
layout(binding=1) uniform sampler shadow_smp;

in vec3 v_wpos;
in vec3 v_nrm;
centroid in vec2 v_uv;

out vec4 frag_color;

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

void main() {
    vec3 base = base_color.rgb;
    if (flags.y > 0.5) {
        vec4 texel = texture(sampler2D(base_tex, base_smp), v_uv * uv_rect.zw + uv_rect.xy);
        if (texel.a < flags.z) {
            discard;
        }
        base *= texel.rgb;
    }
    if (flags.x > 0.5) {
        frag_color = vec4(clamp(base, 0.0, 1.0), 1.0);
        return;
    }
    vec3 n = normalize(v_nrm);
    vec3 light = ambient.rgb;
    int dirs = int(counts.x);
    for (int i = 0; i < 4; i++) {
        if (i >= dirs) {
            break;
        }
        float ndl = max(0.0, dot(n, -dir_dir[i].xyz));
        if (ndl <= 0.0) {
            continue;
        }
        float lit = (i == 0 && counts.z > 0.5) ? shadow_lit(v_wpos, n) : 1.0;
        light += dir_color[i].rgb * (ndl * lit);
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
        float ndl = max(0.0, dot(n, d / dist));
        float fall = 1.0 - dist / range;
        light += point_color[i].rgb * (ndl * fall * fall);
    }
    frag_color = vec4(clamp(base * light, 0.0, 1.0), 1.0);
}
@end

@program mesh mesh_vs mesh_fs

// ----- Position-only passes: shadow map depth and the selection mask -------------

@vs pos_vs
@hlsl_options fixup_clipspace
layout(binding=0) uniform pos_vs_params {
    mat4 mvp;
};

in vec3 position;

void main() {
    gl_Position = mvp * vec4(position, 1.0);
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
};

layout(binding=0) uniform texture2D scene_tex;
layout(binding=0) uniform sampler scene_smp;
layout(binding=1) uniform texture2D mask_tex;
layout(binding=1) uniform sampler mask_smp;

out vec4 frag_color;

void main() {
    vec2 uv = gl_FragCoord.xy / target.xy;
    vec3 c = texture(sampler2D(scene_tex, scene_smp), uv).rgb;
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
