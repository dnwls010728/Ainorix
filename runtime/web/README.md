# Prebuilt web runtime

`oe_player.js` + `oe_player.wasm` are the game player compiled to WebAssembly
(WebGL2, WebAudio). `oe package --web` combines them with
`tools/player/web/index.html` and a game's data (`game.pak`), so making a web
build of a game needs **no Emscripten SDK** — only `oe.exe`.

The runtime does not depend on the game: every project uses the same files.

- Built with: Emscripten 6.0.10, Release (`build_web.bat`), 2026-10-02.
- Engine source: commit b1f5f9e (P4.2 platform gamepads on integrated main).
  Includes standard-mapping Gamepad API polling, stick/trigger axes, named
  buttons, shared 0.15 dead zone, input.axis injection, sim.state inspection,
  Lua input.axis and focus/disconnect reset. Physical controllers remain
  unverified. Windows passed 74 tests and Node/WASM passed 70. P1 saves, P2 skeletal animation
  and P3.1 particle simulation remain included. P3 rendering is a separate PR.
- On this Windows host Binaryen's parallel optimizer crashed; the successful
  rebuild used `BINARYEN_CORES=1`.

## When to rebuild

Rebuild after changing engine C++ code that the player uses (scene format,
components, systems, scripting, physics, rendering, platform/web). Otherwise
web builds keep running the old engine code.

```bat
set EMSDK=C:\path\to\emsdk
build_web.bat
```

`build_web.bat` / `build_web.sh` write the result to `build/bin/web/` and copy
it here. Commit the two files together with the engine change, and update the
lines above.

`oe package --web` prefers a runtime built locally (`build/bin/web/`) over the
one in this folder, and reports which one it used (`webRuntime`).
