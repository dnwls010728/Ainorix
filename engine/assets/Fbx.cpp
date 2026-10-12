#include "assets/Fbx.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <vector>

#include "assets/Assets.h"
#include "core/FileSystem.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "ufbx.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace oe {

namespace {

std::string Str(ufbx_string s) { return std::string(s.data ? s.data : "", s.length); }

Vec3 ToVec3(const ufbx_vec3& v) { return Vec3(static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)); }

// ufbx matrices are affine 3x4, stored by columns like Mat4.
Mat4 ToMat4(const ufbx_matrix& src) {
    Mat4 out;
    for (int col = 0; col < 4; ++col) {
        out.m[col * 4 + 0] = static_cast<float>(src.cols[col].x);
        out.m[col * 4 + 1] = static_cast<float>(src.cols[col].y);
        out.m[col * 4 + 2] = static_cast<float>(src.cols[col].z);
    }
    return out;
}

float Saturate(double value) { return static_cast<float>(std::min(1.0, std::max(0.0, value))); }

// One corner of a triangle. Only 4-byte members, so the struct has no padding and
// ufbx_generate_indices can merge equal corners by comparing bytes.
struct FbxVertex {
    float position[3];
    float normal[3];
    float uv[2];
    float color[4];
    uint32_t vertex;  // index into ufbx_mesh.vertices (the skin weights are stored per vertex)
};

struct FbxLoader {
    Mesh& mesh;
    std::string modelDir;
    std::string& warnings;
    const ufbx_scene* scene = nullptr;
    std::map<const ufbx_texture*, int> textureIndex;
    std::map<const ufbx_material*, int> materialIndex;
    std::map<uint32_t, uint16_t> rigidBindings;
    std::set<uint32_t> animatedNodes;

    void Require(bool condition, const char* message) const {
        if (!condition) throw std::runtime_error(message);
    }

    // Each note is reported once per model.
    void Warn(const std::string& note) {
        if (warnings.find(note) == std::string::npos) warnings += note + "; ";
    }

    void LoadNodes() {
        Require(scene->nodes.count <= 65536, "model exceeds 65536 nodes");
        for (size_t i = 0; i < scene->nodes.count; ++i) {
            const ufbx_node* src = scene->nodes.data[i];
            const ufbx_transform& local = src->local_transform;  // pivots and adjustments are already folded in
            ModelNode node;
            node.name = Str(src->name);
            node.parent = src->parent ? static_cast<int>(src->parent->typed_id) : -1;
            node.translation = ToVec3(local.translation);
            node.rotation = Vec4(static_cast<float>(local.rotation.x), static_cast<float>(local.rotation.y),
                                 static_cast<float>(local.rotation.z), static_cast<float>(local.rotation.w));
            node.scale = ToVec3(local.scale);
            node.matrix = ToMat4(src->node_to_parent);
            for (float value : node.matrix.m) Require(std::isfinite(value), "node transform contains non-finite values");
            mesh.nodes.push_back(node);
        }
    }

    // Appends the keys of one baked channel. A channel that only repeats the node's rest value is left out.
    template <class Keys, class Convert>
    void AddChannel(AnimationClip& clip, uint32_t node, AnimationPath path, const Keys& keys, bool constant, const Vec4& rest,
                    Convert convert) {
        if (keys.count == 0) return;
        AnimationChannel out;
        out.node = static_cast<int>(node);
        out.path = path;
        for (size_t i = 0; i < (constant ? size_t{1} : keys.count); ++i) {
            Vec4 value = convert(keys.data[i].value);
            Require(std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w),
                    "animation value contains non-finite numbers");
            if (path == AnimationPath::Rotation) {
                float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w);
                Require(length > 1e-6f, "animation contains a zero quaternion");
                value = value * (1.0f / length);
            }
            float time = static_cast<float>(std::max(0.0, keys.data[i].time));
            Require(std::isfinite(time), "animation times must be finite");
            // Stepped keys are baked as two keys a fraction of a millisecond apart; keep both in float.
            if (!out.times.empty() && time <= out.times.back()) time = std::nextafter(out.times.back(), std::numeric_limits<float>::max());
            out.times.push_back(time);
            out.values.push_back(value);
        }
        if (constant) {
            Vec4 d = out.values[0] - rest;
            Vec4 s = out.values[0] + rest;  // q and -q are the same rotation
            auto small = [](const Vec4& v) { return std::abs(v.x) + std::abs(v.y) + std::abs(v.z) + std::abs(v.w) < 1e-5f; };
            if (small(d) || (path == AnimationPath::Rotation && small(s))) return;
        }
        clip.duration = std::max(clip.duration, out.times.back());
        animatedNodes.insert(node);
        clip.channels.push_back(std::move(out));
    }

    // FBX curves animate Euler angles with their own tangents; ufbx resamples them into keys
    // that interpolate linearly (rotations spherically), which is what AnimationChannel plays.
    void LoadClips() {
        for (size_t i = 0; i < scene->anim_stacks.count; ++i) {
            const ufbx_anim_stack* stack = scene->anim_stacks.data[i];
            ufbx_bake_opts opts{};
            opts.trim_start_time = true;
            opts.key_reduction_enabled = true;
            opts.key_reduction_rotation = true;
            ufbx_error bakeError{};
            std::unique_ptr<ufbx_baked_anim, void (*)(ufbx_baked_anim*)> bake(ufbx_bake_anim(scene, stack->anim, &opts, &bakeError),
                                                                               ufbx_free_baked_anim);
            if (!bake) {
                Warn("animation '" + Str(stack->name) + "' cannot be read: " + Str(bakeError.description));
                continue;
            }
            AnimationClip clip;
            for (size_t j = 0; j < bake->nodes.count; ++j) {
                const ufbx_baked_node& baked = bake->nodes.data[j];
                if (baked.typed_id >= mesh.nodes.size()) continue;
                const ModelNode& rest = mesh.nodes[baked.typed_id];
                auto vec3 = [](const ufbx_vec3& v) { return Vec4(ToVec3(v), 0); };
                auto quat = [](const ufbx_quat& q) {
                    return Vec4(static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z), static_cast<float>(q.w));
                };
                AddChannel(clip, baked.typed_id, AnimationPath::Translation, baked.translation_keys, baked.constant_translation,
                           Vec4(rest.translation, 0), vec3);
                AddChannel(clip, baked.typed_id, AnimationPath::Rotation, baked.rotation_keys, baked.constant_rotation, rest.rotation, quat);
                AddChannel(clip, baked.typed_id, AnimationPath::Scale, baked.scale_keys, baked.constant_scale, Vec4(rest.scale, 0), vec3);
            }
            if (clip.channels.empty()) continue;  // exporters add an empty default take
            if (std::isfinite(bake->playback_duration)) clip.duration = std::max(clip.duration, static_cast<float>(bake->playback_duration));
            const std::string base = stack->name.length > 0 ? Str(stack->name) : "clip" + std::to_string(i);
            clip.name = base;
            int suffix = 1;
            auto duplicate = [&] {
                return std::any_of(mesh.clips.begin(), mesh.clips.end(), [&](const AnimationClip& c) { return c.name == clip.name; });
            };
            while (duplicate()) clip.name = base + "_" + std::to_string(suffix++);
            mesh.clips.push_back(std::move(clip));
        }
    }

    // Image bytes embedded in the file, else a file beside the model: the stored relative path
    // (never one that climbs out of the model's folder), then just the file name.
    bool LoadImage(const ufbx_texture* texture, Texture& out, std::string* error) {
        if (texture->content.size > 0)
            return DecodeImage(static_cast<const unsigned char*>(texture->content.data), texture->content.size, out, error);
        std::vector<std::string> candidates;
        for (std::string name : {Str(texture->relative_filename), Str(texture->filename)}) {
            std::replace(name.begin(), name.end(), '\\', '/');
            if (name.empty()) continue;
            bool relative = name[0] != '/' && name.find(':') == std::string::npos && ("/" + name + "/").find("/../") == std::string::npos;
            if (relative) candidates.push_back(name);
            candidates.push_back(name.substr(name.find_last_of('/') + 1));
        }
        for (const std::string& candidate : candidates) {
            std::vector<unsigned char> bytes;
            if (candidate.empty() || !ReadBinaryFile(JoinPath(modelDir, candidate), bytes)) continue;
            return DecodeImage(bytes.data(), bytes.size(), out, error);
        }
        if (error) *error = candidates.empty() ? "image has no data" : "cannot read image " + candidates.back();
        return false;
    }

    std::shared_ptr<const Texture> Tex(const ufbx_texture* texture) {
        if (!texture) return nullptr;
        // Layered and procedural textures: use the first file they are built from.
        if (texture->type != UFBX_TEXTURE_FILE) {
            if (texture->file_textures.count == 0) return nullptr;
            texture = texture->file_textures.data[0];
        }
        auto found = textureIndex.find(texture);
        if (found == textureIndex.end()) {
            auto decoded = std::make_shared<Texture>();
            std::string err;
            int index = -1;
            if (LoadImage(texture, *decoded, &err)) {
                index = static_cast<int>(mesh.textures.size());
                mesh.textures.push_back(decoded);
            } else {
                Warn("texture: " + err);
            }
            found = textureIndex.emplace(texture, index).first;
        }
        return found->second >= 0 ? mesh.textures[static_cast<size_t>(found->second)] : nullptr;
    }

    // ufbx fills the PBR maps for every shading model (Lambert/Phong values are converted).
    int MaterialFor(const ufbx_material* src) {
        if (!src) return -1;
        auto it = materialIndex.find(src);
        if (it != materialIndex.end()) return it->second;
        const ufbx_material_pbr_maps& p = src->pbr;
        Material m;
        if (p.base_color.has_value) {
            float factor = p.base_factor.has_value ? Saturate(p.base_factor.value_real) : 1.0f;
            m.baseColor = Color(Saturate(p.base_color.value_vec3.x), Saturate(p.base_color.value_vec3.y), Saturate(p.base_color.value_vec3.z)) * factor;
        }
        m.baseTexture = Tex(p.base_color.texture);
        // An image replaces the color in FBX; the engine multiplies them.
        if (m.baseTexture) m.baseColor = Color(1, 1, 1);
        if (p.metalness.has_value) m.metallic = Saturate(p.metalness.value_real);
        if (p.roughness.has_value) m.roughness = Saturate(p.roughness.value_real);
        if (p.metalness.texture || p.roughness.texture)
            Warn("separate roughness/metalness images are not used (pack them into a .mat.json metallicRoughnessTexture)");
        m.normalTexture = Tex(p.normal_map.texture);
        m.occlusionTexture = Tex(p.ambient_occlusion.texture);
        m.emissiveTexture = Tex(p.emission_color.texture);
        if (m.emissiveTexture) {
            m.emissive = Color(1, 1, 1);
        } else if (p.emission_color.has_value) {
            m.emissive = Color(Saturate(p.emission_color.value_vec3.x), Saturate(p.emission_color.value_vec3.y), Saturate(p.emission_color.value_vec3.z));
        } else {
            m.emissive = Color(0, 0, 0);
        }
        if (p.emission_factor.has_value && std::isfinite(p.emission_factor.value_real))
            m.emissiveIntensity = static_cast<float>(std::max(0.0, p.emission_factor.value_real));
        if (p.opacity.has_value) m.opacity = Saturate(p.opacity.value_real);
        if (m.opacity < 0.999f) m.alphaMode = AlphaMode::Blend;
        else if (p.opacity.texture && Tex(p.opacity.texture) == m.baseTexture && m.baseTexture) m.alphaMode = AlphaMode::Mask;  // cutout in the base image's alpha
        m.doubleSided = src->features.double_sided.enabled;
        m.unlit = src->features.unlit.enabled;
        int index = static_cast<int>(mesh.materials.size());
        mesh.materials.push_back(std::move(m));
        materialIndex[src] = index;
        return index;
    }

    // Meshes of one character usually bind the same bones with the same matrices: share the palette entry.
    uint16_t JointFor(int node, const Mat4& inverseBind) {
        for (size_t i = 0; i < mesh.joints.size(); ++i) {
            if (mesh.joints[i].node != node) continue;
            bool same = true;
            for (size_t k = 0; k < 16 && same; ++k) {
                float a = mesh.joints[i].inverseBind.m[k], b = inverseBind.m[k];
                same = std::abs(a - b) <= 1e-4f * std::max(1.0f, std::max(std::abs(a), std::abs(b)));
            }
            if (same) return static_cast<uint16_t>(i);
        }
        Require(mesh.joints.size() < Mesh::kMaxJoints, "model exceeds the 64-entry skin palette limit");
        for (float value : inverseBind.m) Require(std::isfinite(value), "inverse bind matrix contains non-finite values");
        mesh.joints.push_back({node, inverseBind});
        return static_cast<uint16_t>(mesh.joints.size() - 1);
    }

    void AddMesh(const ufbx_node* node) {
        const ufbx_mesh* src = node->mesh;
        if (src->num_triangles == 0) return;
        const ufbx_skin_deformer* skin = src->skin_deformers.count > 0 ? src->skin_deformers.data[0] : nullptr;
        if (src->skin_deformers.count > 1) Warn("only the first skin of a mesh is used");
        if (src->blend_deformers.count > 0) Warn("blend shapes are not supported");
        // Geometry is baked at the node's rest transform, so a model without a playing clip shows
        // its bind pose; the inverse bind matrices below undo the bake before a joint applies.
        const Mat4 world = ToMat4(node->geometry_to_world);
        const Mat4 normalMatrix = world.Inverse().Transposed();
        const float* m = world.m;
        const float det = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);

        int rigidJoint = -1;
        std::vector<int> clusterJoint;  // palette entry per skin cluster, created on first use
        ufbx_matrix worldToGeometry{};
        if (skin) {
            clusterJoint.assign(skin->clusters.count, -1);
            worldToGeometry = ufbx_matrix_invert(&node->geometry_to_world);
        } else {
            bool animated = false;
            for (const ufbx_node* ancestor = node; ancestor; ancestor = ancestor->parent)
                animated = animated || animatedNodes.count(ancestor->typed_id) != 0;
            if (animated) {
                auto found = rigidBindings.find(node->typed_id);
                if (found == rigidBindings.end()) {
                    Require(mesh.joints.size() < Mesh::kMaxJoints, "model exceeds the 64-entry skin palette limit");
                    uint16_t binding = static_cast<uint16_t>(mesh.joints.size());
                    mesh.joints.push_back({static_cast<int>(node->typed_id), world.Inverse()});
                    found = rigidBindings.emplace(node->typed_id, binding).first;
                }
                rigidJoint = found->second;
            }
        }
        auto jointOfCluster = [&](uint32_t index) -> uint16_t {
            Require(index < clusterJoint.size(), "skin weight names a missing cluster");
            if (clusterJoint[index] < 0) {
                const ufbx_skin_cluster* cluster = skin->clusters.data[index];
                Require(cluster->bone_node != nullptr, "skin cluster has no bone");
                ufbx_matrix inverseBind = ufbx_matrix_mul(&cluster->geometry_to_bone, &worldToGeometry);
                clusterJoint[index] = JointFor(static_cast<int>(cluster->bone_node->typed_id), ToMat4(inverseBind));
            }
            return static_cast<uint16_t>(clusterJoint[index]);
        };

        const bool hasColor = src->vertex_color.exists;
        std::vector<uint32_t> triangle(src->max_face_triangles * 3);
        std::vector<FbxVertex> corners;
        std::vector<uint32_t> cornerIndex;
        for (size_t partIndex = 0; partIndex < src->material_parts.count; ++partIndex) {
            const ufbx_mesh_part& part = src->material_parts.data[partIndex];
            if (part.num_triangles == 0) continue;
            corners.clear();
            for (size_t f = 0; f < part.face_indices.count; ++f) {
                const ufbx_face face = src->faces.data[part.face_indices.data[f]];
                const uint32_t count = ufbx_triangulate_face(triangle.data(), triangle.size(), src, face);
                for (uint32_t k = 0; k < count * 3; ++k) {
                    const uint32_t index = triangle[k];
                    FbxVertex v;
                    std::memset(&v, 0, sizeof(v));
                    const ufbx_vec3 p = ufbx_get_vertex_vec3(&src->vertex_position, index);
                    ufbx_vec3 n;
                    n.x = 0; n.y = 1; n.z = 0;
                    if (src->vertex_normal.exists) n = ufbx_get_vertex_vec3(&src->vertex_normal, index);
                    v.position[0] = static_cast<float>(p.x); v.position[1] = static_cast<float>(p.y); v.position[2] = static_cast<float>(p.z);
                    v.normal[0] = static_cast<float>(n.x); v.normal[1] = static_cast<float>(n.y); v.normal[2] = static_cast<float>(n.z);
                    if (src->vertex_uv.exists) {
                        const ufbx_vec2 uv = ufbx_get_vertex_vec2(&src->vertex_uv, index);
                        v.uv[0] = static_cast<float>(uv.x);
                        v.uv[1] = 1.0f - static_cast<float>(uv.y);  // FBX puts V = 0 at the bottom
                    }
                    if (hasColor) {
                        const ufbx_vec4 c = ufbx_get_vertex_vec4(&src->vertex_color, index);
                        v.color[0] = static_cast<float>(c.x); v.color[1] = static_cast<float>(c.y);
                        v.color[2] = static_cast<float>(c.z); v.color[3] = static_cast<float>(c.w);
                    }
                    v.vertex = src->vertex_indices.data[index];
                    for (float value : v.position) Require(std::isfinite(value), "vertex position contains non-finite values");
                    corners.push_back(v);
                }
            }
            if (corners.empty()) continue;
            cornerIndex.resize(corners.size());
            ufbx_vertex_stream stream{corners.data(), corners.size(), sizeof(FbxVertex)};
            ufbx_error indexError{};
            const size_t unique = ufbx_generate_indices(&stream, 1, cornerIndex.data(), cornerIndex.size(), nullptr, &indexError);
            Require(indexError.type == UFBX_ERROR_NONE, "cannot index the mesh");

            const uint32_t base = static_cast<uint32_t>(mesh.positions.size());
            if (hasColor && mesh.colors.size() < base) mesh.colors.resize(base, Vec4(1, 1, 1, 1));
            for (size_t i = 0; i < unique; ++i) {
                const FbxVertex& v = corners[i];
                mesh.positions.push_back(world.TransformPoint(Vec3(v.position[0], v.position[1], v.position[2])));
                mesh.normals.push_back(Normalize(normalMatrix.TransformDir(Vec3(v.normal[0], v.normal[1], v.normal[2]))));
                mesh.uvs.push_back(v.uv[0]);
                mesh.uvs.push_back(v.uv[1]);
                if (hasColor) mesh.colors.push_back(Vec4(v.color[0], v.color[1], v.color[2], v.color[3]));
                else if (!mesh.colors.empty()) mesh.colors.push_back(Vec4(1, 1, 1, 1));
                SkinVertex influence;
                if (skin && v.vertex < skin->vertices.count) {
                    // Weights are sorted from the strongest: keep four and renormalize.
                    const ufbx_skin_vertex weights = skin->vertices.data[v.vertex];
                    float total = 0;
                    for (uint32_t k = 0; k < std::min<uint32_t>(weights.num_weights, 4); ++k) {
                        const ufbx_skin_weight weight = skin->weights.data[weights.weight_begin + k];
                        const float value = static_cast<float>(weight.weight);
                        if (!(value > 0) || !std::isfinite(value)) continue;
                        influence.joints[k] = jointOfCluster(weight.cluster_index);
                        influence.weights[k] = value;
                        total += value;
                    }
                    if (total > 0) for (float& weight : influence.weights) weight /= total;
                } else if (rigidJoint >= 0) {
                    influence.joints[0] = static_cast<uint16_t>(rigidJoint);
                    influence.weights[0] = 1;
                }
                mesh.skin.push_back(influence);
            }
            Submesh sub;
            sub.firstIndex = static_cast<uint32_t>(mesh.indices.size());
            for (size_t i = 0; i + 2 < cornerIndex.size(); i += 3) {
                const uint32_t a = base + cornerIndex[i], b = base + cornerIndex[i + 1], c = base + cornerIndex[i + 2];
                // Mirrored transforms flip the winding.
                if (det < 0) mesh.indices.insert(mesh.indices.end(), {a, c, b});
                else mesh.indices.insert(mesh.indices.end(), {a, b, c});
            }
            sub.indexCount = static_cast<uint32_t>(mesh.indices.size()) - sub.firstIndex;
            // A node may override the materials of the mesh it instances.
            const ufbx_material* material = part.index < node->materials.count ? node->materials.data[part.index]
                                          : part.index < src->materials.count ? src->materials.data[part.index] : nullptr;
            sub.material = MaterialFor(material);
            mesh.submeshes.push_back(sub);
        }
    }
};

}  // namespace

bool LoadFbxModel(const std::string& path, Mesh& out, std::string& warnings, std::string* error) {
    std::vector<unsigned char> bytes;
    if (!ReadBinaryFile(path, bytes)) {
        if (error) *error = "I/O error";
        return false;
    }
    ufbx_load_opts opts{};
    opts.file_format = UFBX_FILE_FORMAT_FBX;
    // Engine space: right-handed, Y up, meters. The unit is scaled into the vertices and
    // translations, so node scales stay as authored.
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0f;
    opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    // What plain TRS hierarchies cannot express (geometric transforms, pivots, scale-inheritance
    // modes) becomes helper nodes or is folded into the local transforms.
    opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_HELPER_NODES;
    opts.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_COMPENSATE;
    opts.pivot_handling = UFBX_PIVOT_HANDLING_ADJUST_TO_PIVOT;
    opts.generate_missing_normals = true;
    opts.clean_skin_weights = true;
    ufbx_error loadError{};
    std::unique_ptr<ufbx_scene, void (*)(ufbx_scene*)> scene(ufbx_load_memory(bytes.data(), bytes.size(), &opts, &loadError), ufbx_free_scene);
    if (!scene) {
        if (error) {
            *error = "cannot read FBX: " + Str(loadError.description);
            if (loadError.info_length > 0) *error += " (" + std::string(loadError.info, loadError.info_length) + ")";
        }
        return false;
    }
    FbxLoader loader{out, ParentPath(path), warnings, scene.get(), {}, {}, {}, {}};
    try {
        loader.LoadNodes();
        loader.LoadClips();
        for (size_t i = 0; i < scene->nodes.count; ++i) {
            if (scene->nodes.data[i]->mesh) loader.AddMesh(scene->nodes.data[i]);
        }
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
    }
    return true;
}

}  // namespace oe
