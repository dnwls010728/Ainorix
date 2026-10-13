#include "scene/SpriteClip.h"
#include <algorithm>
#include <cmath>

namespace oe {
size_t SpriteClipTiming::Index(double time) const {
    if (starts.empty()) return 0;
    const double tolerance = variable ? 1e-7 : 1e-4 / fps;
    const auto next = std::upper_bound(starts.begin(), starts.end(), time + tolerance);
    return next == starts.begin() ? 0 : static_cast<size_t>(next - starts.begin() - 1);
}
bool ParseSpriteClipTiming(const Json& clip, SpriteClipTiming& out, std::string* error) {
    out = SpriteClipTiming{};
    auto fail = [&](const char* message) { if (error) *error = message; return false; };
    const Json& frames = clip["frames"];
    if (!frames.isArray() || frames.size() == 0 || frames.size() > 65536) return fail("clip frames needs 1..65536 indices");
    out.fps = std::max(0.001, clip["fps"].asNumber(8));
    if (!std::isfinite(out.fps)) return fail("clip fps must be finite");
    out.variable = clip.has("durations");
    const Json& values = clip["durations"];
    if (out.variable && (!values.isArray() || values.size() != frames.size())) return fail("durations needs one value per sequence frame");
    for (size_t i = 0; i < frames.size(); ++i) {
        const double duration = out.variable ? values[i].asNumber(-1) : 1 / out.fps;
        if (out.variable && (!values[i].isNumber() || !std::isfinite(duration) || duration < 0.0001 || duration > 3600))
            return fail("frame durations must be seconds in 0.0001..3600");
        out.starts.push_back(out.duration); out.durations.push_back(duration); out.duration += duration;
    }
    if (out.variable && out.duration > 3600) return fail("variable clip duration exceeds 3600 seconds");
    if (!out.variable) {
        for (size_t i = 0; i < out.starts.size(); ++i) out.starts[i] = static_cast<double>(i) / out.fps;
        out.duration = static_cast<double>(frames.size()) / out.fps;
    }
    out.end = out.variable ? out.duration : out.starts.back();
    return true;
}
bool SpriteClipMarkers(const Json& data, const SpriteClipTiming& timing,
                       std::vector<AnimationMarker>& out, std::string* error) {
    if (!ParseAnimationMarkers(data, static_cast<double>(timing.starts.size()), 1, timing.starts.size(), out, error)) return false;
    for (AnimationMarker& marker : out) marker.time = timing.starts[static_cast<size_t>(marker.time)];
    return true;
}
}  // namespace oe
