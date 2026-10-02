#pragma once
#include <array>

#include "scene/Systems.h"

namespace oe {

// Portable device snapshot. Axes use InputState::kAxisNames order; positive Y is up.
struct GamepadSnapshot {
    bool connected = false;
    int device = -1;
    std::array<float, 6> axes{};
    std::array<bool, 14> buttons{};
};

// Applies physical input without erasing API-injected input when no device is active.
// Each window owns an instance, including the buttons it must release on disconnect.
class GamepadInput {
public:
    inline static constexpr const char* kButtonNames[] = {
        "GamepadA", "GamepadB", "GamepadX", "GamepadY", "GamepadLB", "GamepadRB",
        "GamepadStart", "GamepadBack", "GamepadLeftStick", "GamepadRightStick",
        "GamepadDPadUp", "GamepadDPadDown", "GamepadDPadLeft", "GamepadDPadRight"
    };

    void Apply(InputState& input, const GamepadSnapshot& snapshot) {
        if (!snapshot.connected) { Reset(input); return; }
        if (active_ && device_ != snapshot.device) Reset(input);
        active_ = true;
        device_ = snapshot.device;
        for (size_t i = 0; i < snapshot.axes.size(); ++i) {
            if (!input.SetAxis(InputState::kAxisNames[i], snapshot.axes[i])) input.SetAxis(InputState::kAxisNames[i], 0);
        }
        for (size_t i = 0; i < snapshot.buttons.size(); ++i) {
            const char* name = kButtonNames[i];
            if (snapshot.buttons[i]) {
                if (!input.IsDown(name)) input.pressedThisFrame.insert(name);
                input.down.insert(name);
            } else if (held_[i]) {
                input.down.erase(name);
                input.pressedThisFrame.erase(name);
            }
        }
        held_ = snapshot.buttons;
    }

    // A neutral frame clears only device-owned state; unrelated keys/touches remain intact.
    void Reset(InputState& input) {
        if (!active_) return;
        for (const char* name : InputState::kAxisNames) input.SetAxis(name, 0);
        for (size_t i = 0; i < held_.size(); ++i) if (held_[i]) {
            input.down.erase(kButtonNames[i]);
            input.pressedThisFrame.erase(kButtonNames[i]);
        }
        held_.fill(false);
        active_ = false;
        device_ = -1;
    }

private:
    bool active_ = false;
    int device_ = -1;
    std::array<bool, 14> held_{};
};

}  // namespace oe
