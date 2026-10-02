# Web multi-touch

## Implementation status (work log)

P5 branches from integrated main; the independent P3 rendering and P4 input
PRs are not prerequisites for web touch events.

- [x] P5.1: Queue every changed browser touch, preserve identifiers and normalized
      coordinates, report began for one simulation step, and retain first-finger
      mouse compatibility. Test simultaneous controls, moves, release/cancel and
      focus reset on native and WebAssembly builds.
      Windows passes 73 tests; Node/WASM passes 69. WebTouchLifecycle covers
      press order, identifiers, Lua began flags, simultaneous UIButton keys,
      moves, unknown moves, secondary release, primary cancel, ID reuse and reset.
- [x] P5.2: Rebuild the prebuilt web player and verify browser DOM touch events
      reach Lua/UI; update feature docs and three READMEs.
      An ignored packaged test project used synthetic DOM Touch/TouchEvent
      events through the compiled Emscripten callbacks. Lua observed two
      fingers (IDs 54 and 7), two begins and simultaneous Left/Space/MouseLeft.
      Moving the second finger released only Space without another begin;
      ending it retained Left/MouseLeft. Cancelling the first released all;
      starting another pair then dispatching window blur cleared fingers/keys.
      Canvas coordinates normalized correctly with a 640x360 CSS canvas and
      higher-resolution drawing buffer; the WebGL2 screenshot was inspected.
      The check also exposed captured child-element blur, fixed by listening
      to non-capturing window blur. Clicking between page controls no longer
      clears the gesture. This verifies synthetic DOM events, not touch hardware.
      Native compilation emitted existing EditorTiles/Editor member-shadow
      warnings; the changed platform/header/test code introduced no warnings.
- [ ] Hardware follow-up: exercise real multi-touch on mobile browsers, including
      dragging outside the canvas and switching away while fingers are held.

## Contract

Web touches use the same InputState and Lua input.touches() representation as
Android and API input.touch injection. Coordinates are normalized against the
canvas CSS size, independent of its drawing-buffer pixel ratio. Stable IDs
identify fingers; their list preserves press order. Only changed points in a
DOM event update finger state. Moves do not create unknown fingers or repeat
began. Touchend/touchcancel release only their changed fingers; window blur
clears all fingers and held input.
Page-element focus changes do not count as window blur. The event contract uses
[Emscripten's touch fields](https://emscripten.org/docs/api_reference/html5.h.html#touch),
including isChanged and targetX/targetY.

The first finger in a gesture also drives MouseLeft and the mouse position.
Other fingers never move that mouse or release it. Once the first finger lifts,
remaining fingers continue through input.touches() without taking over the
mouse; the next gesture after all fingers lift chooses a new first finger.
This avoids synthesizing a second mouse click from an already-held finger.
