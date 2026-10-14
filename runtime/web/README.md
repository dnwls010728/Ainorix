# Prebuilt web runtime

`oe_player.js` + `oe_player.wasm` are the shared WebAssembly/WebGL2/WebAudio player.
`oe package --web` combines them with `index.html` and `game.pak`; packaging needs no Emscripten.

## Current build

Built on 2026-10-14 with Emscripten 6.0.10, Release, from main a696524 plus the 2D baseline
source changes committed with these artifacts (ROADMAP T1, T4, T5, T8, T9, T10): named sprite atlases,
Y sorting, tile layers with transformed/animated cells and rule data, pixel-perfect cameras, colored
2D lights through a cached light map (sprites and tilemaps, occluders, normal maps), tiled/sliced/filled sprites as
meshes and per-pixel sprite masks. Includes the earlier
A1-A5 animation features, R9 SSR and earlier rendering/physics/network features.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| oe_player.js | 127,633 | 01cab76470319e28cb0ab53673bf1628a9256d96a4bd2704f3153de0186b9a88 |
| oe_player.wasm | 6,516,822 | 1709d3fbf41c1cd76d5544d2aec53d4bd3c5464f50b1bf940717b462f2c06967 |

Verification: a packaged 2D demo (pixel-perfect 320x180 camera, atlas hero with named walk clip,
Y-sorted trees and wall layer, animated water, lit sprites and tiles with wall shadows, sliced/filled/masked sprites)
ran in headless Edge WebGL2 (SwiftShader) without script/WebGL errors and matched the Windows renderers.

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
