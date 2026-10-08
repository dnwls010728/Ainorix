#pragma once

#include <string>
#include <vector>

#include "core/Json.h"

namespace oe {
// Build-time snapshot of the acceptance assets; data includes binary models and textures.
struct VfxPresetFile {
    std::string path;
    std::string data;
};
// Generated from the explicit VFXLab release manifest, with normalized JSON newlines.
const std::vector<VfxPresetFile>& VfxPresetFiles();
// Catalog entries retain the complete asset document, including parameters and system budget.
Json VfxAcceptancePresets();
}  // namespace oe
