# Prebuilt web runtime

`oe_player.js` + `oe_player.wasm` are the shared WebAssembly/WebGL2/WebAudio player.
`oe package --web` combines them with `index.html` and `game.pak`; packaging needs no Emscripten.

## Current build

Built on 2026-10-14 with Emscripten 6.0.10, Release, from main 8511b48 plus the
live-controller observation source change committed with these artifacts. Includes the earlier
A1-A5 animation playback/events, shared assets, variable frame durations and official oe.animator,
along with R9 SSR and earlier rendering/physics/network features.

This refresh embeds the oe.animator weak owner registry and bounded 16-transition trace, plus
animation.controller_state: read-only state, elapsed time, parameters and captured condition values.
Public Controller:state() results and controller file formats are unchanged. Owner identity and
observations restore through native snapshots. The live graph visualization is editor-only;
the shared C++ bridge and embedded Lua module require rebuilt players.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| oe_player.js | 127,633 | 01cab76470319e28cb0ab53673bf1628a9256d96a4bd2704f3153de0186b9a88 |
| oe_player.wasm | 6,282,178 | 34bffed27f0042e464a75e1c0f3cb9ba0fed5e931901a387b163e6e8b8840368 |

Verification: the freshly packaged model/sheet/atlas controller demo ran in headless Edge WebGL2
(SwiftShader) through Walk, Run, Jump and Idle without script/WebGL errors. Windows controller
observation tests cover read-only isolation, failure atomicity, trigger capture, weak ownership,
bounded history and native rollback. This refresh does not change render algorithms or packaging.

Not verified on real browser/mobile GPUs: performance, audio/touch and minimum-capability shader
paths. Earlier device and feature checks remain in [ANIMATION.md](../../docs/ANIMATION.md) and
[PLATFORMS.md](../../docs/PLATFORMS.md); earlier runtime build history is in Git.

## Rebuild

Rebuild after changing engine C++ or embedded official Lua modules used by the player. Commit both files with the source change and update the
table above. Project scene, Lua and asset edits need packaging, not a runtime build.

```bat
set EMSDK=C:\path\to\emsdk
set BINARYEN_CORES=1
build_web.bat
```

The build writes `build/bin/web` and `runtime/web`. Packaging prefers `build/bin/web` over this committed fallback
and reports the selected `webRuntime`. Binaryen's parallel optimizer crashes on some Windows hosts, hence
`BINARYEN_CORES=1`. To repeat the Wasm test suite, configure `build-web` with `-DOE_BUILD_TESTS=ON`, build
`oe_tests` and run `node build-web/bin/oe_tests.js`.
