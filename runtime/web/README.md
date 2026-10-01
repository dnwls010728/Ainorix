# Prebuilt web runtime

`oe_player.js` + `oe_player.wasm` are the game player compiled to WebAssembly
(WebGL2, WebAudio). `oe package --web` combines them with
`tools/player/web/index.html` and a game's data (`game.pak`), so making a web
build of a game needs **no Emscripten SDK** — only `oe.exe`.

The runtime does not depend on the game: every project uses the same files.

- Built with: Emscripten 6.0.10, Release (`build_web.bat`), 2026-10-02.
- Engine source: commit 91fd4d5 (P3.2 particle billboards, on combined P1/P2/P3.1).
  Includes persistent player storage, skeletal animation and deterministic
  2D/3D camera-facing particles with lifetime color/size/opacity and sheet frames.
  Windows passed 74 tests; Node/WASM passed 70, including matching fixed particle
  frame hashes. D3D11/software particle PNGs were inspected. Browser execution of
  this milestone remains pending; Node GPU tests skip without a canvas.
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
