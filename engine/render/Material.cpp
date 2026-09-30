#include "render/Material.h"

#include <cstdlib>

namespace oe {

namespace {

struct FieldDoc {
    const char* name;
    const char* doc;
};

const FieldDoc kFields[] = {
    {"format", "\"ownengine.material\" (optional)"},
    {"baseColor", "Albedo, [r,g,b] 0..1 or \"#rrggbb\""},
    {"opacity", "Alpha 0..1; below 1 needs alphaMode blend (set automatically)"},
    {"baseTexture", "Image multiplied with baseColor (its alpha with opacity)"},
    {"metallic", "0 = dielectric (plastic, wood, stone), 1 = metal"},
    {"roughness", "0 = mirror-smooth, 1 = fully rough"},
    {"metallicRoughnessTexture", "glTF layout: green = roughness, blue = metallic (multiplied with the factors)"},
    {"normalTexture", "Tangent-space normal map (+Y up, OpenGL/glTF convention)"},
    {"normalScale", "Strength of the normal map"},
    {"occlusionTexture", "Ambient occlusion (red channel), darkens ambient light"},
    {"occlusionStrength", "0..1"},
    {"emissive", "Light the surface gives off, [r,g,b]"},
    {"emissiveIntensity", "Multiplier for emissive (can exceed 1)"},
    {"emissiveTexture", "Image multiplied with emissive"},
    {"alphaMode", "opaque | mask (cut out below alphaCutoff) | blend (transparent)"},
    {"alphaCutoff", "Threshold for mask"},
    {"doubleSided", "Draw back faces too (leaves, cloth, glass panes)"},
    {"unlit", "Ignore lights: color = base (+ emissive)"},
    {"pixelArt", "Nearest-neighbour texture sampling"},
    {"tiling", "UV repeat [x,y]"},
    {"offset", "UV offset [x,y]"},
};

bool ReadColor(const Json& v, Color& c) {
    if (v.isArray() && v.size() >= 3) {
        c = Color(v[0].asFloat(), v[1].asFloat(), v[2].asFloat());
        return true;
    }
    if (v.isString()) {
        std::string s = v.asString();
        if (s.size() == 7 && s[0] == '#') {
            char* end = nullptr;
            unsigned long x = std::strtoul(s.c_str() + 1, &end, 16);
            if (end && *end == '\0') {
                c = Color(((x >> 16) & 0xFF) / 255.0f, ((x >> 8) & 0xFF) / 255.0f, (x & 0xFF) / 255.0f);
                return true;
            }
        }
    }
    return false;
}

}  // namespace

const char* ToString(AlphaMode mode) {
    switch (mode) {
        case AlphaMode::Opaque: return "opaque";
        case AlphaMode::Mask: return "mask";
        case AlphaMode::Blend: return "blend";
    }
    return "opaque";
}

bool MaterialFromJson(const Json& json, const TextureLoader& loadTexture, Material& out, std::string* error) {
    auto fail = [&](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };
    if (!json.isObject()) return fail("a material must be a JSON object");
    Material m;
    bool opacitySet = false;
    for (const auto& kv : json.members()) {
        const std::string& k = kv.first;
        const Json& v = kv.second;
        auto number = [&](float& dst) {
            if (!v.isNumber()) return fail("'" + k + "' must be a number");
            dst = v.asFloat();
            return true;
        };
        auto boolean = [&](bool& dst) {
            if (!v.isBool()) return fail("'" + k + "' must be true or false");
            dst = v.asBool();
            return true;
        };
        auto color = [&](Color& dst) { return ReadColor(v, dst) ? true : fail("'" + k + "' must be [r,g,b] or \"#rrggbb\""); };
        auto pair = [&](float* dst) {
            if (!v.isArray() || v.size() < 2) return fail("'" + k + "' must be [x, y]");
            dst[0] = v[0].asFloat();
            dst[1] = v[1].asFloat();
            return true;
        };
        auto texture = [&](std::shared_ptr<const Texture>& dst) {
            if (!v.isString()) return fail("'" + k + "' must be an image path");
            if (v.asString().empty()) return true;
            std::string err;
            dst = loadTexture ? loadTexture(v.asString(), &err) : nullptr;
            if (!dst) return fail("'" + k + "': " + (err.empty() ? "cannot load " + v.asString() : err));
            return true;
        };
        bool ok = true;
        if (k == "format") ok = v.isString();
        else if (k == "baseColor") ok = color(m.baseColor);
        else if (k == "opacity") ok = number(m.opacity), opacitySet = true;
        else if (k == "baseTexture") ok = texture(m.baseTexture);
        else if (k == "metallic") ok = number(m.metallic);
        else if (k == "roughness") ok = number(m.roughness);
        else if (k == "metallicRoughnessTexture") ok = texture(m.metallicRoughnessTexture);
        else if (k == "normalTexture") ok = texture(m.normalTexture);
        else if (k == "normalScale") ok = number(m.normalScale);
        else if (k == "occlusionTexture") ok = texture(m.occlusionTexture);
        else if (k == "occlusionStrength") ok = number(m.occlusionStrength);
        else if (k == "emissive") ok = color(m.emissive);
        else if (k == "emissiveIntensity") ok = number(m.emissiveIntensity);
        else if (k == "emissiveTexture") ok = texture(m.emissiveTexture);
        else if (k == "alphaMode") {
            std::string s = v.asString("");
            if (s == "opaque") m.alphaMode = AlphaMode::Opaque;
            else if (s == "mask") m.alphaMode = AlphaMode::Mask;
            else if (s == "blend") m.alphaMode = AlphaMode::Blend;
            else ok = fail("'alphaMode' must be \"opaque\", \"mask\" or \"blend\"");
        } else if (k == "alphaCutoff") ok = number(m.alphaCutoff);
        else if (k == "doubleSided") ok = boolean(m.doubleSided);
        else if (k == "unlit") ok = boolean(m.unlit);
        else if (k == "pixelArt") ok = boolean(m.pixelArt);
        else if (k == "tiling") ok = pair(m.tiling);
        else if (k == "offset") ok = pair(m.offset);
        else ok = fail("unknown material field '" + k + "'");
        if (!ok) {
            if (error && error->empty()) *error = "'" + k + "' has the wrong type";
            return false;
        }
    }
    m.opacity = Clamp(m.opacity, 0.0f, 1.0f);
    m.metallic = Clamp(m.metallic, 0.0f, 1.0f);
    m.roughness = Clamp(m.roughness, 0.0f, 1.0f);
    if (opacitySet && m.opacity < 1.0f && m.alphaMode == AlphaMode::Opaque) m.alphaMode = AlphaMode::Blend;
    out = std::move(m);
    return true;
}

Json DefaultMaterialJson() {
    Material d;
    Json j = Json::MakeObject();
    j["format"] = kMaterialFormat;
    j["baseColor"] = Json(Json::Array{d.baseColor.r, d.baseColor.g, d.baseColor.b});
    j["opacity"] = d.opacity;
    j["baseTexture"] = "";
    j["metallic"] = d.metallic;
    j["roughness"] = d.roughness;
    j["metallicRoughnessTexture"] = "";
    j["normalTexture"] = "";
    j["normalScale"] = d.normalScale;
    j["occlusionTexture"] = "";
    j["occlusionStrength"] = d.occlusionStrength;
    j["emissive"] = Json(Json::Array{0, 0, 0});
    j["emissiveIntensity"] = d.emissiveIntensity;
    j["emissiveTexture"] = "";
    j["alphaMode"] = ToString(d.alphaMode);
    j["alphaCutoff"] = d.alphaCutoff;
    j["doubleSided"] = d.doubleSided;
    j["unlit"] = d.unlit;
    j["pixelArt"] = d.pixelArt;
    j["tiling"] = Json(Json::Array{1, 1});
    j["offset"] = Json(Json::Array{0, 0});
    return j;
}

Json MaterialFieldDocs() {
    Json j = Json::MakeObject();
    for (const FieldDoc& f : kFields) j[f.name] = f.doc;
    return j;
}

}  // namespace oe
