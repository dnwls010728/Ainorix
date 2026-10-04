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
- [x] P4.2: Windows dynamic XInput, browser Gamepad API and Android analog/button
      adapters, disconnect/focus reset and platform builds. Windows Release passes
      74 tests and Node/WASM passes 70. GamepadDeviceLifecycle checks all named buttons, press/hold/release,
      controller replacement, focus reset, disconnect, malformed axis normalization
      and preservation of injected input while no physical device is active.
      Web and both Android ABI players rebuilt without new warnings; runtime
      refresh is recorded in runtime/web/README.md and runtime/android/README.md.
- [x] P4.3: Platformer/Dungeon stick, D-pad, A and Start controls; focused native
      editor Game-view forwarding through commands, with axes/buttons released
      on focus loss. Windows passes 75 tests; Node/WASM passes 71.
      SampleGamepadControls checks partial-stick speed despite Android aliases,
      dead-zone rest, D-pad speed, jump hold/release, firing, restart and repeated
      scene determinism. NativeEditorHeadless checks forwarding and focus reset.
      CLI-injected samples were rendered and inspected (Platformer hash
      34e10cc120feae25, Dungeon d0a265a1f2245fb1); the D3D11 editor screenshot
      confirmed the translated Game input hint and sample control instructions.
      Three READMEs and sample guides updated. Only editor C++/Lua/sample data
      changed after P4.2, so player runtime binaries remain current.
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

Mouse look is separate from the sticks: a local script stores a look direction with Lua
`input.setLook(x, y)` (each -1..1, read with `input.look()`), and a network project carries it by
declaring `LookX`/`LookY` in `network.axes` (no dead zone). See SCRIPTING.md, authoritative gameplay.

input.axis {name, value} injects a raw axis value. It rejects unknown names,
non-finite values and values outside the axis range. sim.state exposes axes
(after dead zone) and rawAxes (before dead zone), including all six names.
Lua input.axis(name) returns the processed value and rejects unknown names.

Buttons use existing input.key injection and Lua input.down/input.pressed:
GamepadA, GamepadB, GamepadX, GamepadY, GamepadLB, GamepadRB, GamepadStart,
GamepadBack, GamepadLeftStick, GamepadRightStick, GamepadDPadUp,
GamepadDPadDown, GamepadDPadLeft and GamepadDPadRight. pressed is true only on
the first simulation step after a new press. Input is transient, not scene undo
or saved scene data.

## Physical device adapters

Each player window maps one controller to the logical input contract. Windows
polls the lowest connected XInput index, dynamically loading XInput from the
system directory; missing DLLs or controllers leave injected input intact.
Signed stick endpoints are normalized to exactly -1 and 1 and triggers to 0..1.

The web player polls Emscripten's Gamepad API bridge while the document is
visible and focused. It selects the lowest connected index with a standard
mapping; devices with an empty or vendor-specific mapping are ignored rather
than guessing button positions. Browser Y axes are inverted to positive-up.
Standard buttons 6/7 supply LT/RT and the standard face/shoulder/menu/stick/D-pad
indices supply the named buttons. A browser may expose controllers only after
a user interacts with one.

Android selects the first device that sends a controller event and retains it
until disconnected. X/Y and Z/RZ supply the two sticks with Y inverted. LT/RT
accept LTRIGGER/RTRIGGER or BRAKE/GAS; L2/R2 key events supply a full-scale fallback
until an analog trigger value is observed. D-pad keys and hat axes are combined.
The previous arrow/Space/Escape/Shift/Control/Enter aliases remain available.
InputDevice.getDevice detects removal; unavailable JNI queries preserve the
current device rather than fabricating a disconnect.

Losing focus or disconnecting releases physical buttons and zeros physical
axes. A controller replacement begins new button presses. With no active device,
polling does not erase API-injected input. While a device is active its sampled
axes overwrite injected axes; injection is intended for headless tools/tests.
Physical polling has been compiled on all three platforms; actual controller
behavior still needs the hardware checks listed above.

## Sample and editor controls

Platformer uses left stick X or the D-pad to move, A to jump (hold for higher)
and Start to restart. Dungeon uses left stick X/Y or the D-pad to move, A to
shoot and Start to restart. Keyboard and on-screen key controls remain usable.
An active stick takes precedence over digital directions so Android's legacy
arrow aliases cannot turn partial analog movement into full speed. Dungeon
caps vectors longer than one to prevent faster diagonal movement, while smaller
stick vectors preserve their analog magnitude.

The native editor forwards window gamepad input only while a play session's
Game view has focus. Forwarding uses input.axis/input.key commands, sends button
state changes without repeating held presses, and releases only forwarded input
when focus is lost. A window with no sampled axes leaves agent-injected axes
alone. Click the Game view to resume forwarding after losing window focus.
