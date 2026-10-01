#include "assets/Assets.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

#include "app/Engine.h"
#include "audio/Wav.h"
#include "render/Material.h"
#include "core/FileSystem.h"
#include "core/Log.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "cgltf.h"
#include "stb_image.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace oe {

namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool EndsWith(const std::string& s, const char* suffix) {
    size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

const char* CgltfError(cgltf_result r) {
    switch (r) {
        case cgltf_result_data_too_short: return "file is truncated";
        case cgltf_result_unknown_format: return "not a glTF/GLB file";
        case cgltf_result_invalid_json: return "invalid glTF JSON";
        case cgltf_result_invalid_gltf: return "invalid glTF structure";
        case cgltf_result_file_not_found: return "referenced file not found (.bin or image)";
        case cgltf_result_io_error: return "I/O error";
        case cgltf_result_out_of_memory: return "out of memory";
        case cgltf_result_legacy_gltf: return "glTF 1.0 is not supported (convert to glTF 2.0)";
        default: return "cannot read glTF";
    }
}

// Loads the image of a glTF texture: embedded (buffer view / data URI) or a file next to the model.
bool LoadGltfImage(const cgltf_image* image, const std::string& modelDir, Texture& out, std::string* error) {
    if (image->buffer_view && image->buffer_view->buffer->data) {
        const unsigned char* data = static_cast<const unsigned char*>(image->buffer_view->buffer->data) + image->buffer_view->offset;
        return DecodeImage(data, image->buffer_view->size, out, error);
    }
    if (!image->uri) {
        if (error) *error = "image has no data";
        return false;
    }
    std::string uri = image->uri;
    if (uri.rfind("data:", 0) == 0) {
        size_t comma = uri.find(";base64,");
        if (comma == std::string::npos) {
            if (error) *error = "unsupported data URI";
            return false;
        }
        std::string b64 = uri.substr(comma + 8);
        size_t size = b64.size() / 4 * 3 - (b64.size() >= 2 && b64[b64.size() - 1] == '=' ? (b64[b64.size() - 2] == '=' ? 2 : 1) : 0);
        void* decoded = nullptr;
        cgltf_options opt{};
        if (cgltf_load_buffer_base64(&opt, size, b64.c_str(), &decoded) != cgltf_result_success) {
            if (error) *error = "bad base64 image";
            return false;
        }
        bool ok = DecodeImage(static_cast<unsigned char*>(decoded), size, out, error);
        std::free(decoded);
        return ok;
    }
    std::vector<char> path(uri.begin(), uri.end());
    path.push_back('\0');
    cgltf_decode_uri(path.data());
    std::vector<unsigned char> bytes;
    if (!ReadBinaryFile(JoinPath(modelDir, path.data()), bytes)) {
        if (error) *error = std::string("cannot read image ") + path.data();
        return false;
    }
    return DecodeImage(bytes.data(), bytes.size(), out, error);
}

struct Loader {
    Mesh& mesh;
    std::string modelDir;
    std::map<const cgltf_image*, int> textureIndex;
    std::string warnings;
    std::map<const cgltf_material*, int> materialIndex;

    std::shared_ptr<const Texture> Tex(const cgltf_texture_view& view) {
        int i = TextureFor(view);
        return i >= 0 ? mesh.textures[static_cast<size_t>(i)] : nullptr;
    }

    // glTF 2.0 metallic-roughness material (+ KHR_materials_emissive_strength, KHR_materials_unlit).
    int MaterialFor(const cgltf_material* src) {
        if (!src) return -1;
        auto it = materialIndex.find(src);
        if (it != materialIndex.end()) return it->second;
        Material m;
        m.metallic = 1.0f;  // glTF defaults
        m.roughness = 1.0f;
        if (src->has_pbr_metallic_roughness) {
            const cgltf_pbr_metallic_roughness& p = src->pbr_metallic_roughness;
            m.baseColor = Color(p.base_color_factor[0], p.base_color_factor[1], p.base_color_factor[2]);
            m.opacity = p.base_color_factor[3];
            m.baseTexture = Tex(p.base_color_texture);
            m.metallic = p.metallic_factor;
            m.roughness = p.roughness_factor;
            m.metallicRoughnessTexture = Tex(p.metallic_roughness_texture);
        } else {
            m.metallic = 0.0f;
            m.roughness = 0.7f;
            if (src->has_pbr_specular_glossiness) warnings += "specular-glossiness materials are shown as base color only; ";
        }
        m.normalTexture = Tex(src->normal_texture);
        if (m.normalTexture) m.normalScale = src->normal_texture.scale;
        m.occlusionTexture = Tex(src->occlusion_texture);
        if (m.occlusionTexture) m.occlusionStrength = src->occlusion_texture.scale;
        m.emissive = Color(src->emissive_factor[0], src->emissive_factor[1], src->emissive_factor[2]);
        m.emissiveTexture = Tex(src->emissive_texture);
        if (src->has_emissive_strength) m.emissiveIntensity = src->emissive_strength.emissive_strength;
        m.alphaMode = src->alpha_mode == cgltf_alpha_mode_blend ? AlphaMode::Blend : src->alpha_mode == cgltf_alpha_mode_mask ? AlphaMode::Mask : AlphaMode::Opaque;
        m.alphaCutoff = src->alpha_cutoff;
        m.doubleSided = src->double_sided;
        m.unlit = src->unlit;
        int index = static_cast<int>(mesh.materials.size());
        mesh.materials.push_back(std::move(m));
        materialIndex[src] = index;
        return index;
    }

    int TextureFor(const cgltf_texture_view& view) {
        if (!view.texture || !view.texture->image) return -1;
        const cgltf_image* image = view.texture->image;
        auto it = textureIndex.find(image);
        if (it != textureIndex.end()) return it->second;
        auto tex = std::make_shared<Texture>();
        std::string err;
        int index = -1;
        if (LoadGltfImage(image, modelDir, *tex, &err)) {
            index = static_cast<int>(mesh.textures.size());
            mesh.textures.push_back(tex);
        } else {
            warnings += "texture: " + err + "; ";
        }
        textureIndex[image] = index;
        return index;
    }

    void AddPrimitive(const cgltf_primitive& prim, const Mat4& world) {
        if (prim.type != cgltf_primitive_type_triangles) return;
        const cgltf_accessor* pos = nullptr;
        const cgltf_accessor* nor = nullptr;
        const cgltf_accessor* uv = nullptr;
        for (cgltf_size i = 0; i < prim.attributes_count; ++i) {
            const cgltf_attribute& a = prim.attributes[i];
            if (a.type == cgltf_attribute_type_position) pos = a.data;
            else if (a.type == cgltf_attribute_type_normal) nor = a.data;
            else if (a.type == cgltf_attribute_type_texcoord && a.index == 0) uv = a.data;
        }
        if (!pos) return;
        uint32_t base = static_cast<uint32_t>(mesh.positions.size());
        Mat4 normalMatrix = world.Inverse().Transposed();
        for (cgltf_size i = 0; i < pos->count; ++i) {
            float p[3] = {0, 0, 0}, n[3] = {0, 1, 0}, t[2] = {0, 0};
            cgltf_accessor_read_float(pos, i, p, 3);
            if (nor) cgltf_accessor_read_float(nor, i, n, 3);
            if (uv) cgltf_accessor_read_float(uv, i, t, 2);
            mesh.positions.push_back(world.TransformPoint(Vec3(p[0], p[1], p[2])));
            mesh.normals.push_back(Normalize(normalMatrix.TransformDir(Vec3(n[0], n[1], n[2]))));
            mesh.uvs.push_back(t[0]);
            mesh.uvs.push_back(t[1]);
        }
        Submesh sub;
        sub.firstIndex = static_cast<uint32_t>(mesh.indices.size());
        // Mirrored transforms flip the winding.
        const float* m = world.m;
        float det = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) + m[8] * (m[1] * m[6] - m[5] * m[2]);
        std::vector<uint32_t> idx;
        if (prim.indices) {
            for (cgltf_size i = 0; i < prim.indices->count; ++i) idx.push_back(base + static_cast<uint32_t>(cgltf_accessor_read_index(prim.indices, i)));
        } else {
            for (cgltf_size i = 0; i < pos->count; ++i) idx.push_back(base + static_cast<uint32_t>(i));
        }
        for (size_t i = 0; i + 2 < idx.size(); i += 3) {
            if (det < 0) mesh.indices.insert(mesh.indices.end(), {idx[i], idx[i + 2], idx[i + 1]});
            else mesh.indices.insert(mesh.indices.end(), {idx[i], idx[i + 1], idx[i + 2]});
        }
        sub.indexCount = static_cast<uint32_t>(mesh.indices.size()) - sub.firstIndex;
        if (!nor) {
            // Smooth normals for this primitive only.
            std::vector<Vec3> acc(pos->count, Vec3(0, 0, 0));
            for (uint32_t i = sub.firstIndex; i + 2 < sub.firstIndex + sub.indexCount; i += 3) {
                uint32_t a = mesh.indices[i], b = mesh.indices[i + 1], c = mesh.indices[i + 2];
                Vec3 fn = Cross(mesh.positions[b] - mesh.positions[a], mesh.positions[c] - mesh.positions[a]);
                acc[a - base] += fn;
                acc[b - base] += fn;
                acc[c - base] += fn;
            }
            for (size_t i = 0; i < acc.size(); ++i) mesh.normals[base + i] = Normalize(acc[i]);
        }
        sub.material = MaterialFor(prim.material);
        mesh.submeshes.push_back(sub);
    }

    void AddNode(const cgltf_node* node) {
        if (node->mesh) {
            // Skinned meshes ignore their node transform (glTF spec); we show the bind pose.
            Mat4 world;
            if (!node->skin) {
                float m[16];
                cgltf_node_transform_world(node, m);
                std::memcpy(world.m, m, sizeof(m));  // both column-major
            }
            for (cgltf_size i = 0; i < node->mesh->primitives_count; ++i) AddPrimitive(node->mesh->primitives[i], world);
        }
        for (cgltf_size i = 0; i < node->children_count; ++i) AddNode(node->children[i]);
    }
};

}  // namespace

bool DecodeImage(const unsigned char* data, size_t size, Texture& out, std::string* error) {
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &channels, 4);
    if (!pixels) {
        if (error) *error = std::string("cannot decode image: ") + stbi_failure_reason();
        return false;
    }
    out.width = w;
    out.height = h;
    out.texels.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
    std::memcpy(out.texels.data(), pixels, out.texels.size() * 4);
    stbi_image_free(pixels);
    return true;
}

bool LoadModelFile(const std::string& path, Mesh& out, std::string* error) {
    cgltf_options options{};
    cgltf_data* data = nullptr;
    cgltf_result r = cgltf_parse_file(&options, path.c_str(), &data);
    if (r == cgltf_result_success) r = cgltf_load_buffers(&options, data, path.c_str());
    if (r != cgltf_result_success) {
        if (error) *error = CgltfError(r);
        if (data) cgltf_free(data);
        return false;
    }
    Loader loader{out, ParentPath(path), {}, {}, {}};
    if (data->scenes_count > 0) {
        const cgltf_scene* scene = data->scene ? data->scene : &data->scenes[0];
        for (cgltf_size i = 0; i < scene->nodes_count; ++i) loader.AddNode(scene->nodes[i]);
    } else {
        for (cgltf_size i = 0; i < data->nodes_count; ++i) {
            if (!data->nodes[i].parent) loader.AddNode(&data->nodes[i]);
        }
    }
    cgltf_free(data);
    if (out.indices.empty()) {
        if (error) *error = "model contains no triangle meshes";
        return false;
    }
    out.ComputeBounds();
    out.ComputeTangents();
    if (!loader.warnings.empty()) OE_LOG_WARN("assets", "%s: %s", path.c_str(), loader.warnings.c_str());
    return true;
}

// ----- AssetManager ---------------------------------------------------------------

std::string AssetManager::KindOf(const std::string& path) {
    std::string p = Lower(path);
    if (EndsWith(p, ".glb") || EndsWith(p, ".gltf")) return "model";
    if (EndsWith(p, ".png") || EndsWith(p, ".jpg") || EndsWith(p, ".jpeg") || EndsWith(p, ".bmp") || EndsWith(p, ".tga")) return "texture";
    if (EndsWith(p, ".wav")) return "audio";
    if (EndsWith(p, ".ttf") || EndsWith(p, ".otf") || EndsWith(p, ".ttc")) return "font";
    if (EndsWith(p, ".lua")) return "script";
    if (EndsWith(p, ".prefab.json")) return "prefab";
    if (EndsWith(p, ".mat.json")) return "material";
    if (EndsWith(p, ".tileset.json")) return "tileset";
    if (EndsWith(p, ".scene.json")) return "scene";
    return "other";
}

AssetManager::Entry<Mesh> AssetManager::LoadMesh(const std::string& path) {
    Entry<Mesh> e;
    try {
        std::string full = engine_.ResolvePath(path);
        e.mtime = FileModifiedTime(full);
        auto mesh = std::make_shared<Mesh>();
        if (!FileExists(full)) e.error = "model file not found: " + path;
        else if (KindOf(path) != "model") e.error = "'" + path + "' is not a built-in mesh (cube, sphere, plane, pyramid, quad) or a .glb/.gltf model";
        else if (LoadModelFile(full, *mesh, &e.error)) e.asset = mesh;
        else e.error = path + ": " + e.error;
    } catch (const std::exception& ex) {
        e.error = ex.what();
    }
    if (!e.error.empty()) OE_LOG_WARN("assets", "%s", e.error.c_str());
    return e;
}

AssetManager::Entry<Texture> AssetManager::LoadTexture(const std::string& path) {
    Entry<Texture> e;
    try {
        std::string full = engine_.ResolvePath(path);
        e.mtime = FileModifiedTime(full);
        std::vector<unsigned char> bytes;
        auto tex = std::make_shared<Texture>();
        if (!ReadBinaryFile(full, bytes)) e.error = "texture file not found: " + path;
        else if (DecodeImage(bytes.data(), bytes.size(), *tex, &e.error)) e.asset = tex;
        else e.error = path + ": " + e.error;
    } catch (const std::exception& ex) {
        e.error = ex.what();
    }
    if (!e.error.empty()) OE_LOG_WARN("assets", "%s", e.error.c_str());
    return e;
}

AssetManager::Entry<Material> AssetManager::LoadMaterial(const std::string& path) {
    Entry<Material> e;
    try {
        std::string full = engine_.ResolvePath(path);
        e.mtime = FileModifiedTime(full);
        std::string text, parseError;
        if (KindOf(path) != "material") e.error = "'" + path + "' is not a material file (*.mat.json)";
        else if (!ReadTextFile(full, text)) e.error = "material file not found: " + path;
        else {
            Json j = Json::parse(text, &parseError);
            auto mat = std::make_shared<Material>();
            TextureLoader load = [this](const std::string& p, std::string* err) { return GetTexture(p, err); };
            if (!parseError.empty()) e.error = path + ": " + parseError;
            else if (MaterialFromJson(j, load, *mat, &e.error)) e.asset = mat;
            else e.error = path + ": " + e.error;
        }
    } catch (const std::exception& ex) {
        e.error = ex.what();
    }
    if (!e.error.empty()) OE_LOG_WARN("assets", "%s", e.error.c_str());
    return e;
}

std::shared_ptr<const Material> AssetManager::GetMaterial(const std::string& path, std::string* error) {
    auto it = materials_.find(path);
    if (it == materials_.end()) it = materials_.emplace(path, LoadMaterial(path)).first;
    if (error) *error = it->second.error;
    return it->second.asset;
}

AssetManager::Entry<Tileset> AssetManager::LoadTileset(const std::string& path) {
    Entry<Tileset> e;
    try {
        std::string full = engine_.ResolvePath(path);
        e.mtime = FileModifiedTime(full);
        std::string text, parseError;
        if (KindOf(path) != "tileset") e.error = "'" + path + "' is not a tileset file (*.tileset.json)";
        else if (!ReadTextFile(full, text)) e.error = "tileset file not found: " + path;
        else {
            Json j = Json::parse(text, &parseError);
            auto ts = std::make_shared<Tileset>();
            if (!parseError.empty()) e.error = path + ": " + parseError;
            else if (ParseTileset(j, *ts, &e.error)) {
                ts->key = path + "@" + std::to_string(e.mtime) + ts->key;
                e.asset = ts;
            } else {
                e.error = path + ": " + e.error;
            }
        }
    } catch (const std::exception& ex) {
        e.error = ex.what();
    }
    if (!e.error.empty()) OE_LOG_WARN("assets", "%s", e.error.c_str());
    return e;
}

std::shared_ptr<const Tileset> AssetManager::GetTileset(const std::string& path, std::string* error) {
    auto it = tilesets_.find(path);
    if (it == tilesets_.end()) it = tilesets_.emplace(path, LoadTileset(path)).first;
    if (error) *error = it->second.error;
    return it->second.asset;
}

TilesetLookup AssetManager::Tilesets() {
    return [this](const std::string& path, std::string* error) { return GetTileset(path, error); };
}

AssetManager::FontEntry AssetManager::LoadFont(const std::string& path) {
    FontEntry e;
    try {
        std::string full = engine_.ResolvePath(path);
        e.mtime = FileModifiedTime(full);
        std::vector<unsigned char> bytes;
        if (KindOf(path) != "font") e.error = "'" + path + "' is not a font (\"default\", \"pixel\" or a .ttf/.otf/.ttc file)";
        else if (!ReadBinaryFile(full, bytes)) e.error = "font file not found: " + path;
        else if (!(e.asset = FontFace::Load(std::move(bytes), &e.error))) e.error = path + ": " + e.error;
    } catch (const std::exception& ex) {
        e.error = ex.what();
    }
    if (!e.error.empty()) OE_LOG_WARN("assets", "%s", e.error.c_str());
    return e;
}

std::shared_ptr<FontFace> AssetManager::GetFont(const std::string& path, std::string* error) {
    if (path.empty() || path == "default") return FontFace::Default();
    auto it = fonts_.find(path);
    if (it == fonts_.end()) it = fonts_.emplace(path, LoadFont(path)).first;
    if (error) *error = it->second.error;
    return it->second.asset;
}

std::shared_ptr<const Mesh> AssetManager::GetMesh(const std::string& name, std::string* error) {
    if (const Mesh* builtin = GetBuiltinMesh(name)) {
        return std::shared_ptr<const Mesh>(std::shared_ptr<const Mesh>(), builtin);  // non-owning
    }
    auto it = meshes_.find(name);
    if (it == meshes_.end()) it = meshes_.emplace(name, LoadMesh(name)).first;
    if (error) *error = it->second.error;
    return it->second.asset;
}

std::shared_ptr<const Texture> AssetManager::GetTexture(const std::string& path, std::string* error) {
    auto it = textures_.find(path);
    if (it == textures_.end()) it = textures_.emplace(path, LoadTexture(path)).first;
    if (error) *error = it->second.error;
    return it->second.asset;
}

std::vector<std::string> AssetManager::PollChanges() {
    std::vector<std::string> changed;
    auto mtimeOf = [&](const std::string& path) -> int64_t {
        try {
            return FileModifiedTime(engine_.ResolvePath(path));
        } catch (const std::exception&) {
            return 0;
        }
    };
    for (auto& kv : meshes_) {
        if (mtimeOf(kv.first) != kv.second.mtime) {
            kv.second = LoadMesh(kv.first);
            changed.push_back(kv.first);
        }
    }
    for (auto& kv : textures_) {
        if (mtimeOf(kv.first) != kv.second.mtime) {
            kv.second = LoadTexture(kv.first);
            changed.push_back(kv.first);
        }
    }
    // Materials reload when their file changes or a texture was reloaded (they hold the old one).
    bool texturesChanged = !changed.empty();
    for (auto& kv : materials_) {
        if (texturesChanged || mtimeOf(kv.first) != kv.second.mtime) {
            bool fileChanged = mtimeOf(kv.first) != kv.second.mtime;
            kv.second = LoadMaterial(kv.first);
            if (fileChanged) changed.push_back(kv.first);
        }
    }
    for (auto& kv : fonts_) {
        if (mtimeOf(kv.first) != kv.second.mtime) {
            kv.second = LoadFont(kv.first);
            changed.push_back(kv.first);
        }
    }
    for (auto& kv : tilesets_) {
        if (mtimeOf(kv.first) != kv.second.mtime) {
            kv.second = LoadTileset(kv.first);
            changed.push_back(kv.first);
        }
    }
    for (const std::string& p : changed) OE_LOG_INFO("assets", "reloaded %s", p.c_str());
    return changed;
}

void AssetManager::Forget(const std::string& path) {
    meshes_.erase(path);
    textures_.erase(path);
    materials_.erase(path);
    fonts_.erase(path);
    tilesets_.erase(path);
}

void AssetManager::Clear() {
    meshes_.clear();
    textures_.clear();
    materials_.clear();
    fonts_.clear();
    tilesets_.clear();
}

Json AssetManager::Info(const std::string& path) {
    Json out = Json::MakeObject();
    out["path"] = path;
    std::string kind = GetBuiltinMesh(path) ? "model" : KindOf(path);
    out["kind"] = kind;
    if (!GetBuiltinMesh(path)) {
        std::string full = engine_.ResolvePath(path);
        std::vector<unsigned char> bytes;
        if (!ReadBinaryFile(full, bytes)) throw ApiError("not_found", "cannot read '" + path + "'", "Call asset.list to see project files.");
        out["bytes"] = static_cast<uint64_t>(bytes.size());
    } else {
        out["builtin"] = true;
    }
    if (kind == "model") {
        std::string err;
        auto mesh = GetMesh(path, &err);
        if (!mesh) throw ApiError("invalid_model", err);
        out["vertices"] = static_cast<uint64_t>(mesh->positions.size());
        out["triangles"] = static_cast<uint64_t>(mesh->TriangleCount());
        Json subs = Json::MakeArray();
        for (const Submesh& s : mesh->submeshes) {
            Json j = Json::MakeObject();
            j["triangles"] = s.indexCount / 3;
            j["material"] = s.material;
            subs.push(j);
        }
        out["submeshes"] = subs;
        Json mats = Json::MakeArray();
        for (const Material& m : mesh->materials) {
            Json j = Json::MakeObject();
            j["baseColor"] = Json(Json::Array{m.baseColor.r, m.baseColor.g, m.baseColor.b});
            j["opacity"] = m.opacity;
            j["metallic"] = m.metallic;
            j["roughness"] = m.roughness;
            j["alphaMode"] = ToString(m.alphaMode);
            Json maps = Json::MakeArray();
            if (m.baseTexture) maps.push("base");
            if (m.metallicRoughnessTexture) maps.push("metallicRoughness");
            if (m.normalTexture) maps.push("normal");
            if (m.occlusionTexture) maps.push("occlusion");
            if (m.emissiveTexture) maps.push("emissive");
            j["textures"] = maps;
            if (m.doubleSided) j["doubleSided"] = true;
            if (m.unlit) j["unlit"] = true;
            mats.push(j);
        }
        out["materials"] = mats;
        Json texs = Json::MakeArray();
        for (const auto& t : mesh->textures) texs.push(Json(Json::Array{t->width, t->height}));
        out["textures"] = texs;
        Vec3 size = mesh->boundsMax - mesh->boundsMin;
        out["boundsMin"] = Json(Json::Array{mesh->boundsMin.x, mesh->boundsMin.y, mesh->boundsMin.z});
        out["boundsMax"] = Json(Json::Array{mesh->boundsMax.x, mesh->boundsMax.y, mesh->boundsMax.z});
        out["size"] = Json(Json::Array{size.x, size.y, size.z});
        float largest = std::max(size.x, std::max(size.y, size.z));
        if (largest > 0) out["hint"] = Format("Largest extent is %.3g m; Transform.scale %.3g makes it about 1.8 m tall.", largest, 1.8f / std::max(size.y, 1e-6f));
    } else if (kind == "texture") {
        std::string err;
        auto tex = GetTexture(path, &err);
        if (!tex) throw ApiError("invalid_texture", err);
        out["width"] = tex->width;
        out["height"] = tex->height;
    } else if (kind == "material") {
        std::string err;
        auto mat = GetMaterial(path, &err);
        if (!mat) throw ApiError("invalid_material", err, "Fields: see material.create / docs/RENDERING.md.");
        std::string text;
        ReadTextFile(engine_.ResolvePath(path), text);
        out["values"] = Json::parse(text);
        out["alphaMode"] = ToString(mat->alphaMode);
    } else if (kind == "font") {
        std::string err;
        auto font = GetFont(path, &err);
        if (!font) throw ApiError("invalid_font", err);
        out["family"] = font->familyName;
        out["hint"] = Format("Use it with UIText/UIButton {font: \"%s\"}. Characters the font lacks fall back to the built-in font.", path.c_str());
    } else if (kind == "tileset") {
        std::string err;
        auto ts = GetTileset(path, &err);
        if (!ts) throw ApiError("invalid_tileset", err, "Format: {image, columns, rows, tiles: {\"#\": rule}}; see docs/2D.md.");
        out["image"] = ts->image;
        out["columns"] = ts->columns;
        out["rows"] = ts->rows;
        std::string chars;
        for (const auto& kv : ts->tiles) chars += kv.first;
        out["tiles"] = chars;
        out["hint"] = Format("Use it with Tilemap {tileset: \"%s\"}; the map characters %s are defined by the file.", path.c_str(), chars.c_str());
    } else if (kind == "audio") {
        std::vector<unsigned char> bytes;
        ReadBinaryFile(engine_.ResolvePath(path), bytes);
        AudioClip clip;
        std::string err;
        if (!DecodeWav(bytes, clip, &err)) throw ApiError("invalid_audio", err);
        out["seconds"] = clip.Seconds();
    }
    return out;
}

}  // namespace oe
