# Prebuilt web runtime

`oe_player.js` + `oe_player.wasm` are the shared WebAssembly/WebGL2/WebAudio player.
`oe package --web` combines them with `index.html` and `game.pak`; packaging needs no Emscripten.

## Current build

Built on 2026-10-08 with Emscripten 6.0.10, Release, `BINARYEN_CORES=1`, parallelism 2, from branch
`claude/rpg-village` (after main `7cc9d8b`). It contains every engine feature up to then: scripting and official
Lua modules, physics (Jolt, Box2D), UI widgets, saves, animation, gamepads and multi-touch, host bridge,
networking (WebSocket client, protocol v4), asset-based VFX with expressions, surface shader graphs (vertex offset,
normal, `Sprite.material`), post effects (tone mapping incl. ACES, bloom, DOF, FXAA, fog, colour grading),
hemisphere ambient, `Sprite.emission`, glTF `COLOR_0` and the `LeftBracket`/`RightBracket` key names.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| oe_player.js | 127,442 | 1e9089cf64fde3dcbfdd5de9afb8b8e9496151abf2eb13ad7e491636f12669cc |
| oe_player.wasm | 4,952,310 | 1d43af71f0b825e5ebaab64792640e54f25144f392863c102d946b33ec19e496 |

Windows Release: all engine tests pass. Not verified in a browser for this build: GPU execution of the newer
shaders (DOF, graph vertex stage, fog, grading), typing into `UIInput`, real ads, sound output and phone touch.
A browser can join binary WebSocket sessions but cannot host or use raw UDP/TCP (`link.open` returns `nil`).
Earlier per-build notes are in the git history of this file.

## Rebuild

Rebuild after changing engine C++ used by the player. Commit both files with the source change and update the
table above. Scene, Lua and asset edits need packaging, not a runtime build.

```bat
set EMSDK=C:\path\to\emsdk
set BINARYEN_CORES=1
build_web.bat
```

The build writes `build/bin/web` and `runtime/web`. Packaging prefers `build/bin/web` over this committed fallback
and reports the selected `webRuntime`. Binaryen's parallel optimizer crashes on some Windows hosts, hence
`BINARYEN_CORES=1`. To repeat the Wasm test suite, configure `build-web` with `-DOE_BUILD_TESTS=ON`, build
`oe_tests` and run `node build-web/bin/oe_tests.js`.
