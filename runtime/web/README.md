# Prebuilt web runtime

`oe_player.js` + `oe_player.wasm` are the shared WebAssembly/WebGL2/WebAudio player.
`oe package --web` combines them with index.html and game.pak; packaging needs no Emscripten.

- Built: 2026-10-03, Emscripten 6.0.10, Release (`build_web.bat`, BINARYEN_CORES=1).
- Source: the network audit follow-up (P7 editor source plus the fixes listed under
  "Audit follow-up" in docs/NETWORK.md): both fallback paths always received, single-step
  client interpolation, departed players dropped from authoritative matches, pending
  connections outside player slots, deferred save flush committed when a match ends, lockstep
  catch-up for stalled peers.
  Includes protocol v4 sessions (binary snapshot payloads on the unreliable channel), lockstep/native rollback, authoritative replication/prediction,
  Lua lobby controls, loopback previews/fault controls, network on-screen action sampling,
  saves, skeletal animation, particles, gamepads, multi-touch, postprocessing and surface
  shader graphs as before.
- Validation: Windows 154 tests; Wasm/Node 141 registered tests, zero failed checks (native
  sockets and GPU tests skip). JS callback bounds/cleanup tests pass. The packaged browser
  client against a native WebSocket server was last exercised with the previous build
  (NetArena join/ready/move/collect/reset); it was not repeated for this rebuild.
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
