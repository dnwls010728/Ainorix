#pragma once
#include <algorithm>
#include <optional>

#include "scene/Systems.h"

namespace oe {

// Applies ordered browser touch changes; coordinates are already normalized to the view.
class TouchInput {
public:
    enum class Kind { Begin, Move, End };

    // Only Begin creates a finger. Repeated begins/moves preserve the one-step began flag.
    void Apply(InputState& input, Kind kind, int id, float x, float y) {
        auto finger = std::find_if(input.touches.begin(), input.touches.end(),
                                   [id](const InputState::Touch& touch) { return touch.id == id; });
        if (kind == Kind::End) {
            if (finger != input.touches.end()) input.touches.erase(finger);
            if (primary_ && *primary_ == id) {
                input.mouseX = x;
                input.mouseY = y;
                input.down.erase("MouseLeft");
                input.pressedThisFrame.erase("MouseLeft");
                primary_.reset();
            }
            return;
        }
        if (finger == input.touches.end()) {
            if (kind != Kind::Begin) return;
            if (input.touches.empty()) primary_ = id;
            input.touches.push_back({id, x, y, true});
        } else {
            finger->x = x;
            finger->y = y;
        }
        if (primary_ && *primary_ == id) {
            input.mouseX = x;
            input.mouseY = y;
            if (!input.IsDown("MouseLeft")) input.pressedThisFrame.insert("MouseLeft");
            input.down.insert("MouseLeft");
        }
    }

    // Window blur ends the gesture, including a mouse press and unconsumed edge.
    void Reset(InputState& input) {
        input.touches.clear();
        if (primary_) {
            input.down.erase("MouseLeft");
            input.pressedThisFrame.erase("MouseLeft");
        }
        primary_.reset();
    }

private:
    std::optional<int> primary_;  // remaining fingers never synthesize a second click
};

}  // namespace oe
