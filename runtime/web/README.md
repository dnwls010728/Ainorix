# Prebuilt web runtime

`oe_player.js` + `oe_player.wasm` are the game player compiled to WebAssembly
(WebGL2, WebAudio). `oe package --web` combines them with
`tools/player/web/index.html` and a game's data (`game.pak`), so making a web
build of a game needs **no Emscripten SDK** — only `oe.exe`.

The runtime does not depend on the game: every project uses the same files.

- Built with: Emscripten 6.0.10, Release (`build_web.bat`), 2026-10-02.
- Engine source: commit 0ea84f2 (P6.3a directional FXAA and highlight bloom).
  Includes PostProcess settings, optional vignette, HDR scene buffers, exposure
  and Reinhard tone mapping plus two-pass highlight bloom in both renderers,
  optional directional FXAA, UI/selection separation and neutral default behavior,
  particle billboards/sample effects, gamepad input
  and web multi-touch. Windows tests pass 83 cases; Node/WASM passes 79.
  D3D11/software comparison and real CLI screenshots were checked.
  Browser WebGL2 verifies Reinhard, neutral/HDR transitions and zero exposure
  with HUD separation. Bloom on/off, canvas resizing and vignette/neutral
  transitions are also verified through the packaged WebGL2 player.
  FXAA is tested on Windows and WASM; packaged WebGL2 FXAA and sample
  controls remain pending in docs/POSTPROCESS.md.
  P1 saves, P2 skeletal animation and P3.1 particle simulation remain included.
  P3 rendering, P4 gamepads and P5 touch adapters are included in this runtime.
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
