# Prebuilt web runtime

`oe_player.js` + `oe_player.wasm` are the shared WebAssembly/WebGL2/WebAudio player.
`oe package --web` combines them with index.html and game.pak; packaging needs no Emscripten.

- Built: 2026-10-03, Emscripten 6.0.10, Release (`build_web.bat`, BINARYEN_CORES=1).
- Source: the networking M8a/M8b commit containing this README (base 5a3f714 plus M8 changes).
  Includes protocol v3 sessions, lockstep/native rollback, authoritative replication/prediction,
  Lua lobby controls, loopback previews/fault controls and network on-screen action sampling.
  Existing saves, skeletal animation, particles, gamepads, multi-touch and postprocessing remain
  included. Portable surface shader graph/compiler/material binding from the current source
  is included too; packaged browser shader-graph visual verification remains separately pending.
- Validation: Windows 135 tests; Wasm/Node 123 registered tests, zero failed checks (native
  sockets and GPU tests skip). Real packaged NetArena WebGL2 browser client joined a native
  WebSocket dedicated server, readied, moved, collected a crystal, received score/HUD and reset
  the round with zero Lua errors. JS callback bounds/cleanup tests pass.
- Browser can join binary WebSocket sessions, but cannot host or use raw UDP/TCP. Android
  runtimes are separate and remain pending. See [NETWORK_SAMPLES.md](../../docs/NETWORK_SAMPLES.md).
- Binaryen's parallel optimizer crashes on this Windows host; use BINARYEN_CORES=1.

## Rebuild

Rebuild after changing engine C++ used by the player. Commit both files with the source
change and update this README. Source/Lua/art-only game edits need packaging, not a runtime build.

```bat
set EMSDK=C:\path\to\emsdk
set BINARYEN_CORES=1
build_web.bat
```

The build writes build/bin/web and runtime/web. Packaging prefers build/bin/web over this
committed fallback and reports the selected webRuntime. Enable OE_BUILD_TESTS in build-web,
build oe_tests and run `node build-web/bin/oe_tests.js` to repeat the Wasm suite.
