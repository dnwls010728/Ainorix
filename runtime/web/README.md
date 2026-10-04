# Prebuilt web runtime

`oe_player.js` + `oe_player.wasm` are the shared WebAssembly/WebGL2/WebAudio player.
`oe package --web` combines them with index.html and game.pak; packaging needs no Emscripten.

- Built: 2026-10-04, Emscripten 6.0.10, Release (`build_web.bat`, BINARYEN_CORES=1).
- Source: the Dongle Zoo branch plus the UI widget work (`UIButton` textures/icons/toggles, keyboard/gamepad
  focus, `UIInput` with typed text from `KeyboardEvent.key`, caret/selection and an in-game clipboard,
  `UIDropdown`, `UIJoystick`, images on every UI element): host bridge (`host.call` / `host.poll` / `host.language`, the page's
  `window.OEHost` and `Module.oeHostSend`), `input.viewSize`, on top of the VFX toolkit (particle
  curves, `Trail`), the Water/HD-2D work and everything listed before (protocol v4 sessions, lockstep,
  rollback, authoritative replication, saves, animation, gamepads, multi-touch, postprocessing,
  surface shader graphs).
- Validation: Windows 194 tests. The Wasm test suite was not rebuilt for this runtime, and the UI widget work
  (typing into a `UIInput` in a browser in particular) was not run in a browser. Before that work, the packaged
  `games/DongleZoo` was driven in headless Edge (WebGL2 through SwiftShader) over the DevTools
  protocol: page load, `host.js` loaded (`window.OEHost`, `Module.oeHostSend` present), Play clicked,
  animals dropped and merged, save written to localStorage, a host message queued and drained by the
  game. Real ads (AdSense account), sound output and touch on a phone browser were not checked.
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
