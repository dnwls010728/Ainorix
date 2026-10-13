# Prebuilt web runtime

`oe_player.js` + `oe_player.wasm` are the shared WebAssembly/WebGL2/WebAudio player.
`oe package --web` combines them with `index.html` and `game.pak`; packaging needs no Emscripten.

## Current build

Built on 2026-10-13 with Emscripten 6.0.10, Release, from main (27aba99). R6 adds
PlanarReflection on flat MeshRenderer surfaces: one visible receiver per view, a reflected camera,
shared oblique clipping and a current-scene capture that includes moving characters. Captures do not
recurse or include UI. The light-data texture is now 8x19 RGBA32F. All earlier runtime features remain
included; see [PLANAR_REFLECTION.md](../../docs/PLANAR_REFLECTION.md).
Packaging still patches the game's key into the runtime for keyed OEPAK002 archives and stripped Lua bytecode.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| oe_player.js | 127,633 | 01cab76470319e28cb0ab53673bf1628a9256d96a4bd2704f3153de0186b9a88 |
| oe_player.wasm | 6,022,336 | 55a9da1565522e94befbfdc45db3478e3e7a1b03310280afa43584747db72434 |

Windows Release: 354 tests pass, including planar selection/bounds, transformed planes, motion,
behind-plane clipping, horizontal/vertical and perspective/orthographic parity, and unchanged depth/IDs.
The 800x500 demo's software/D3D11 mean RGB difference is 0.373/255 (0.388 after 90 simulation frames).
Headless Edge (WebGL2 on SwiftShader) compiled and linked the generated mesh program and rendered
the packaged animated demonstration without shader/WebGL errors. The fragment program uses 209 vec4
uniform vectors and 15 samplers (224 conservatively), with 15 texture units, within GLES3/WebGL2 minimums.
No new vertex-stage resources; the existing skin/vertex-graph minimum-device path remains unverified.
Earlier browser checks are in Git history.

Not verified on real browser GPUs or phones: frame rate, UIInput typing, sound output, touch, ads, and
newer animation/physics/networking paths beyond the earlier recorded browser checks. A browser can join
binary WebSocket sessions but cannot host or use raw UDP/TCP (link.open returns nil).

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
