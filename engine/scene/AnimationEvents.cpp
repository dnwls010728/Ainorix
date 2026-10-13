#include "scene/AnimationEvents.h"
#include <algorithm>
#include <cmath>
#include "api/Commands.h"

namespace oe {

bool ParseAnimationMarkers(const Json& data, double duration, double fps, size_t frames,
                           std::vector<AnimationMarker>& out, std::string* error) {
    out.clear();
    auto fail = [&](const char* message) { if (error) *error = message; return false; };
    if (data.isNull()) return true;
    if (!data.isArray() || data.size() > kMaxAnimationMarkers) return fail("events must be an array of at most 256 markers");
    for (const Json& item : data.items()) {
        const char* field = fps > 0 ? "frame" : "time";
        const double value = item[field].asNumber(-1);
        const std::string name = item["name"].asString();
        if (!item.isObject() || !item[field].isNumber() || !std::isfinite(value) || value < 0 ||
            name.empty() || name.size() > 128 || name == "finished") return fail("marker needs a finite nonnegative time/frame and a name (1..128 bytes, not finished)");
        for (const auto& member : item.members())
            if (member.first != field && member.first != "name") return fail("unknown event field; use only time/frame and name");
        if (fps > 0 && (std::floor(value) != value || value >= static_cast<double>(frames)))
            return fail("event frame must be an integer index inside the clip's frames array");
        const double time = fps > 0 ? value / fps : value;
        if (time > duration) return fail("event time exceeds the clip duration");
        out.push_back({time, name});
    }
    std::stable_sort(out.begin(), out.end(), [](const AnimationMarker& a, const AnimationMarker& b) { return a.time < b.time; });
    return true;
}

void CollectAnimationEvents(EntityId id, const std::string& clip, const std::vector<AnimationMarker>& markers,
                            double start, double end, double duration, bool loop, bool first,
                            std::vector<AnimationEvent>& out) {
    if (markers.empty()) return;
    const bool reverse = end < start;
    struct Crossing { double at; size_t marker; };
    std::vector<Crossing> crossings;
    auto add = [&](double at, size_t index) {
        if (crossings.size() == kMaxAnimationEventsPerStep)
            throw ApiError("animation_event_limit", "more than 4096 animation events crossed in one step", "Lower playback speed or marker count.");
        crossings.push_back({at, index});
    };
    for (size_t i = 0; i < markers.size(); ++i) {
        const double t = markers[i].time;
        if (loop && duration > 0) {
            // Enumerate marker occurrences, not empty cycles: large speeds without markers stay cheap.
            double cycle = reverse ? std::ceil((start - t) / duration) - 1 : std::floor((start - t) / duration) + 1;
            if (first && start == t) cycle += reverse ? 1 : -1;
            for (size_t n = 0; n <= kMaxAnimationEventsPerStep; ++n) {
                const double at = t + cycle * duration;
                if (reverse ? at < end : at > end) break;
                add(at, i);
                cycle += reverse ? -1 : 1;
            }
        } else if (reverse ? ((t < start || (first && t == start)) && t >= end)
                           : ((t > start || (first && t == start)) && t <= end)) add(t, i);
    }
    std::stable_sort(crossings.begin(), crossings.end(), [&](const Crossing& a, const Crossing& b) {
        if (a.at != b.at) return reverse ? a.at > b.at : a.at < b.at;
        // At a seam, finish this cycle before entering the next. Equal-time markers keep authored order.
        if (markers[a.marker].time != markers[b.marker].time)
            return reverse ? markers[a.marker].time < markers[b.marker].time : markers[a.marker].time > markers[b.marker].time;
        return a.marker < b.marker;
    });
    for (const Crossing& crossing : crossings)
        out.push_back({id, markers[crossing.marker].name, clip, std::fabs(crossing.at - start)});
}

}  // namespace oe
