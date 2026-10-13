#pragma once
#include <string>
#include <vector>
#include "core/Json.h"
#include "scene/Reflect.h"

namespace oe {

// Authored timeline marker in seconds, kept in stable time/source order.
struct AnimationMarker { double time = 0; std::string name; };
// A fixed-step notification; offset orders markers from two playback components on one entity.
struct AnimationEvent { EntityId id = kNullEntity; std::string name, clip; double offset = 0; };
inline constexpr size_t kMaxAnimationMarkers = 256;
inline constexpr size_t kMaxAnimationEventsPerStep = 4096;

// Strict marker validation: model {time,name}; sprite {frame,name}, frame is a clip-sequence index.
bool ParseAnimationMarkers(const Json& data, double duration, double fps, size_t frames,
                           std::vector<AnimationMarker>& out, std::string* error);
// Collects all crossings including repeated loops; the first advance includes the starting marker.
// Throws animation_event_limit before playback changes if this component crosses more than 4096 markers.
void CollectAnimationEvents(EntityId id, const std::string& clip, const std::vector<AnimationMarker>& markers,
                            double start, double end, double duration, bool loop, bool first,
                            std::vector<AnimationEvent>& out);

}  // namespace oe
