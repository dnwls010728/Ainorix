#pragma once
#include <functional>
#include <memory>
#include <string>

#include "core/Json.h"
#include "render/Mesh.h"

namespace oe {

// Material files (`*.mat.json`): a JSON object with any of the fields below;
// missing fields keep their defaults. Texture fields hold project paths.
//
//   baseColor [r,g,b] | "#rrggbb", opacity, baseTexture,
//   metallic, roughness, metallicRoughnessTexture,
//   normalTexture, normalScale, occlusionTexture, occlusionStrength,
//   emissive, emissiveIntensity, emissiveTexture,
//   alphaMode "opaque" | "mask" | "blend", alphaCutoff,
//   doubleSided, unlit, pixelArt, tiling [x,y], offset [x,y]
constexpr const char* kMaterialFormat = "ownengine.material";

using TextureLoader = std::function<std::shared_ptr<const Texture>(const std::string& path, std::string* error)>;

// Reads a material description. Unknown fields and wrong types are errors
// (with the field name); textures that fail to load are errors too.
bool MaterialFromJson(const Json& json, const TextureLoader& loadTexture, Material& out, std::string* error);

// The default material as JSON (every field), for new material files.
Json DefaultMaterialJson();

// Field name -> short description, for docs and error hints.
Json MaterialFieldDocs();

const char* ToString(AlphaMode mode);

}  // namespace oe
