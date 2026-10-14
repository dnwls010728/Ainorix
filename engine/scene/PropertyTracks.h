#pragma once
// Property tracks: keyframed component fields carried by an animation clip (sprite clips inline or in
// .anim.json, model clips in <model>.clips.json metadata), applied by the clip's playback each fixed step.
//
//   "tracks": [{"target": "Sword", "component": "Transform", "field": "rotation", "interpolation": "linear",
//               "keys": [{"time": 0, "value": [0, 0, 0]}, {"time": 0.2, "value": [0, 0, -90]}]}]
//
// target: "" = the animated entity itself, else a path of child names ("Arm/Hand"). interpolation: linear
// (default), smooth (eased in and out between keys) or step; numbers, vectors and colors interpolate per
// component (rotations in degrees), other field types (int, bool, string, frame) always step. Before the
// first key the first value holds, after the last key the last one.
#include <string>

#include "core/Json.h"
#include "scene/Reflect.h"

namespace oe {

class Scene;

inline constexpr size_t kMaxPropertyTracks = 64;
inline constexpr size_t kMaxPropertyTrackKeys = 512;

// Field types a track can animate (Json and entity references cannot).
bool AnimatableField(const FieldInfo& field);
// Strict check of a clip's "tracks" (shape, limits, known component/field, key values of the field's type,
// times in increasing order within 0..3600 s). The animation components themselves cannot be targets.
bool ValidatePropertyTracks(const Json& tracks, std::string* error);
// The entity a track names, from the clip's owner; kNullEntity when no such child exists.
EntityId PropertyTrackTarget(const Scene& scene, EntityId owner, const std::string& target);
// A track's value at `time` seconds into the clip; null when it has no keys. `stepOnly` samples without
// interpolation (fields that cannot blend).
Json SamplePropertyTrack(const Json& track, double time, bool stepOnly);
// Writes every track's value at `time` into the components of the owner (or its children), in authored
// order. Tracks whose entity, component or field is missing, or whose value does not fit, are skipped.
void ApplyPropertyTracks(Scene& scene, EntityId owner, const Json& tracks, double time);

}  // namespace oe
