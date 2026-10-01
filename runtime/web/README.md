# Prebuilt web runtime

`oe_player.js` + `oe_player.wasm` are the game player compiled to WebAssembly
(WebGL2, WebAudio). `oe package --web` combines them with
`tools/player/web/index.html` and a game's data (`game.pak`), so making a web
build of a game needs **no Emscripten SDK** — only `oe.exe`.

The runtime does not depend on the game: every project uses the same files.

- Built with: Emscripten 6.0.10, Release (`build_web.bat`), 2026-10-02.
- Engine source: commit adc8dc0 plus Android runtime verification changes.
  Includes multi-touch simulation, `UIButton.key`, App Bundle support, the
  unused game selection pass optimization, reliable Lua reload after API writes
  and Windows drive-path handling. The WebAssembly/Node suite passed 58 tests
  (GPU tests skip without a browser canvas).
- On this Windows host Binaryen's parallel optimizer crashed; the successful
  rebuild used `BINARYEN_CORES=1`.
- P1 refresh: JSON save slots, strict Lua value validation, explicit tool save
  directories and player localStorage. Browser reload restores saved counters
  (0/1 then 1/2) on the save-probe HUD. Source: P1 branch after aad02f2.

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
