#pragma once
#include <string>
#include <vector>

#include "scene/Scene.h"

namespace oe {

// Copies selected subtrees once, using local ids and remapping reflected entity
// references. References outside the selection become null; JSON params stay verbatim.
Json CopyEntities(const Scene& scene, const std::vector<EntityId>& selection);
// Validates an entire clipboard document before committing any entities. Root
// transforms stay local to the requested parent. Returns newly created roots.
bool PasteEntities(Scene& scene, const Json& document, EntityId parent,
                   std::vector<EntityId>& roots, std::string* error);

}  // namespace oe
