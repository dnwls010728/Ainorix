# Gamepad input

## Implementation status (work log)

P4 starts from the integrated main. P3 rendering and sample effects remain in
their separate follow-up PR; gamepad input does not depend on that feature.

- [x] P4.1: Portable raw axes, fixed dead zone, input.axis injection, sim.state
      inspection, Lua input.axis and button edge/state tests. Windows Release passes
      73 tests. GamepadAxesAndButtons drives deterministic Lua movement, all six
      axes, range errors, dead-zone endpoints, button press/hold/repress, input
      persistence, non-undo state and stop reset. The real CLI confirmed raw
      LeftX 0.575 reads as approximately 0.5 through API state and Lua.
      Node/WASM passes 69 tests. Web and both Android players rebuilt without
      new warnings. The PlatformNull translation unit also compiled with
      Emscripten/Clang (existing unused-parameter warnings in Platform.h); this
      is a portability compile check, not a full Linux build or device test.
- [ ] P4.2: Windows dynamic XInput, browser Gamepad API and Android analog/button
      adapters, disconnect/focus reset, platform builds and runtime refresh.
- [ ] P4.3: Gameplay sample controls, documentation/three READMEs and available
      end-to-end verification. Record physical-controller checks separately.
- [ ] Hardware follow-up: verify sticks, triggers, buttons and disconnect on a
      real controller on Windows, web and Android; no device is assumed available.

## Logical input contract

The logical axes are LeftX, LeftY, RightX, RightY, LT and RT. Stick values range
from -1 to 1, with positive X right and positive Y up; triggers range from 0 to 1.
Raw values persist until updated or the play session stops. A fixed scalar dead
zone of 0.15 is applied when reading: magnitudes at or below 0.15 become zero;
the remaining range is rescaled linearly to full scale. Triggers use the same
dead zone. Applying it at the shared read boundary avoids platform differences
and allows tests to inject exactly the values a device would produce.

input.axis {name, value} injects a raw axis value. It rejects unknown names,
non-finite values and values outside the axis range. sim.state exposes axes
(after dead zone) and rawAxes (before dead zone), including all six names.
Lua input.axis(name) returns the processed value and rejects unknown names.

Buttons use existing input.key injection and Lua input.down/input.pressed:
GamepadA, GamepadB, GamepadX, GamepadY, GamepadLB, GamepadRB, GamepadStart,
GamepadBack, GamepadLeftStick, GamepadRightStick, GamepadDPadUp,
GamepadDPadDown, GamepadDPadLeft and GamepadDPadRight. pressed is true only on
the first simulation step after a new press. Input is transient, not scene undo
or saved scene data. Physical device adapters are the next milestone.
