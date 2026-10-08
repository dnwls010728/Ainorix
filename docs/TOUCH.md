# Web multi-touch

The web player reports every finger to Lua (`input.touches()`) and to on-screen controls (`UIButton.key`,
`UIJoystick`); the first finger also acts as the mouse so taps work like clicks. Open items are under Status at
the end.

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

## Status

- [ ] Real multi-touch on mobile browsers, including dragging outside the canvas and switching away while
      fingers are held.

