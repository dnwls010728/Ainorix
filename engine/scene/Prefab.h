#pragma once
#include <string>

#include "core/Json.h"
#include "scene/Scene.h"

namespace oe {

// Prefab file format (prefabs/*.prefab.json):
//   {"format": "ownengine.prefab", "version": 1, "name": "Coin",
//    "entities": [{"id": 1, "name": "Coin", "components": {...}},
//                 {"id": 2, "parent": 1, ...}]}
// entities[0] is the root; ids are local to the file.
constexpr const char* kPrefabFormat = "ownengine.prefab";

// Captures an entity and all of its descendants.
Json MakePrefab(const Scene& scene, EntityId root);

// Creates a copy of the prefab in the scene (fresh ids) and returns the root,
// or kNullEntity with `error` set. The root gets a Prefab component that
// records `prefabPath`.
EntityId InstantiatePrefab(Scene& scene, const Json& prefab, const std::string& prefabPath, EntityId parent, std::string* error);

}  // namespace oe
