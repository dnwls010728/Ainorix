#pragma once
#include "scene/AnimationEvents.h"

namespace oe {
// One deterministic timeline shared by sprites, marker queries, preview and drawing.
struct SpriteClipTiming {
    std::vector<double> starts, durations;
    double duration = 0, end = 0, fps = 8;
    bool variable = false;
    // Clamped sequence index; an exact boundary selects the following frame.
    size_t Index(double time) const;
};
// Explicit durations override fps. Legacy non-looping clips finish at the last frame's start.
bool ParseSpriteClipTiming(const Json& clip, SpriteClipTiming& out, std::string* error);
// Sprite markers keep authored sequence indices and use cumulative frame start times.
bool SpriteClipMarkers(const Json& data, const SpriteClipTiming& timing,
                       std::vector<AnimationMarker>& out, std::string* error);
}  // namespace oe
