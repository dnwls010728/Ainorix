#pragma once
#include <string>

#include "render/Mesh.h"

namespace oe {

// Reads an Autodesk FBX file (binary or ASCII) into `out` the way LoadModelFile reads glTF:
// right-handed, Y up, meters (the file's axes and unit are converted), V flipped to the
// top-left origin, static geometry baked into model space, skins and rigid animated nodes as
// palette joints, every animation stack resampled into linear TRS clips. Textures come from
// the file's embedded media or from image files beside the model. Notes for the log (ignored
// features, unreadable textures) are appended to `warnings`. Fbx.cpp is the only code that
// knows ufbx.
bool LoadFbxModel(const std::string& path, Mesh& out, std::string& warnings, std::string* error);

}  // namespace oe
