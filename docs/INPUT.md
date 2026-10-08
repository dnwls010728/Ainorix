# Gamepad input

Gamepads are read through six logical axes and named buttons that are the same on every platform and can be
injected through the API (`input.axis`, `input.key`), so controller code is testable without a device. What is
still unverified on real hardware is listed under Status at the end.

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

## Official input and math composition (M3)

The embedded oe.input module supplies named project-defined actions without changing
input injection, wire declarations, or the global input table. Define actions in Lua:

```lua
local controls = require("oe.input")
local actions = {
    move = {kind="vector", left={"A","Left"}, right={"D","Right"},
            up={"W","Up"}, down={"S","Down"}, axisX="LeftX", axisY="LeftY"},
    jump = {kind="button", keys={"Space","GamepadA"}},
}
local source = controls.localSource() -- offline; fails during a network session
local move = controls.read(source, actions, "move") -- {x,y,z=0}, +Y up
local jump = controls.read(source, actions, "jump") -- {held,pressed}
```

Aliases in one direction are ORed, opposite directions subtract, processed axes add,
and only vectors longer than 1 are normalized. Smaller analog magnitudes survive.
UIJoystick and a physical pad share axis names; a held UI stick writes those same axes,
so the module does not add a second stick value. The native scalar 0.15 dead zone is
applied once by input.axis/input.player.axis; the module applies none. Digital and
analog directions sum, potentially cancel, and cap to 1. +Y is up; map movement onto
XZ with z=-move.y. Screen/touch Y grows downward. Binding axes are optional; a keyboard-only game can leave them out.

Buttons OR each alias's held and first-frame pressed values independently. Repeated
reads do not consume input. Engine onUpdate keyboard suppression while UIInput is
focused already applies; mouse/gamepad remain active. The same suppression happens
before network sampling. Tool script.eval outside onUpdate is not a suppressed
input callback. Modules do not pause: scripts/UI still run at dt=0 and the gameplay
caller decides whether to move. They do not inspect/consume touches or pointer data.

For network gameplay, explicitly select the entity's player and mirror the project's
network.actions/network.axes declarations (literal engine key names, not named Lua
actions). Only include bound declared names; an undeclared alias fails even if another
alias is held. Example for a game that declares WASD and LeftX/LeftY:

```lua
local declarations = {actions={"W","A","S","D","Space","R"}, axes={"LeftX","LeftY"}}
local actions = {move={kind="vector",left={"A"},right={"D"},up={"W"},down={"S"},
                      axisX="LeftX",axisY="LeftY"}}
local source = controls.playerSource(self:get("NetPlayer").player, declarations)
local move = controls.read(source, actions, "move")
```

playerSource captures native input.player(id) closures and copies allowlists into its
Lua heap closures. It supplies only down/pressed/axis, rejects undeclared reads and
non-running matches (including none/idle/lobby), and never reads device-local mouse/touches/undeclared keys.
Unknown players are neutral. Lockstep/rollback peers have all streams; authoritative
servers have player streams and clients have their own stream (remote is neutral).
localSource refuses any non-none/non-idle session at read time, even if cached before
joining. Lobby UI uses existing global input rather than the gameplay source API.
Neither source guesses a player/role. Declarations remain a project author obligation:
this is an ergonomic boundary, not a security sandbox against arbitrary project Lua
or forged allowlists. Existing global input APIs stay available for local UI.

Tables/actions are caller-owned. Shared modules retain no mutable owner state or
native registry; closures and imported cache are in the recorded Lua heap. Enable
sim.record_state before require/evaluation. A caller may cache its explicit source
on the script instance; no subscriptions or cleanup are needed. Project helper reload
follows SCRIPTING.md Hot reload: instance fields survive, affected module-local state
resets, and captured functions/callbacks may retain old code. Official exports stay immutable.

| oe.math operation | Contract |
|---|---|
| clamp(value, low, high) | Inclusive finite bounds; inverted bounds error |
| lerp(a, b, t) | a+(b-a)*t, t is not clamped |
| distance(a, b) | Euclidean length using {x,y,z}; omitted coordinates zero |
| normalize(v) | Fresh unit {x,y,z}, or exact zero for zero vector; finite coordinates required |

The module does not introduce positional-array vectors, RNG/time consumption, mutable
native caches, or scene changes. normalize rescales before squaring to handle very
large finite coordinates. Arithmetic overflow in distance/lerp follows Lua numbers;
use finite representable gameplay ranges.

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

## Game and editor controls

Recommended game-side handling (as in `games/RPG/scripts/hero.lua` and the test fixture `tests/fixtures/Dungeon`):
an active stick takes precedence over digital directions, so Android's legacy arrow aliases cannot turn partial
analog movement into full speed; cap vectors longer than one to prevent faster diagonal movement, while smaller
stick vectors keep their analog magnitude.

The native editor forwards window gamepad input only while a play session's
Game view has focus. Forwarding uses input.axis/input.key commands, sends button
state changes without repeating held presses, and releases only forwarded input
when focus is lost. A window with no sampled axes leaves agent-injected axes
alone. Click the Game view to resume forwarding after losing window focus.

## Status

- [ ] Real controllers on Windows, web and Android: sticks, triggers, buttons, disconnect.

