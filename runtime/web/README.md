# Prebuilt web runtime

`oe_player.js` + `oe_player.wasm` are the shared WebAssembly/WebGL2/WebAudio player.
`oe package --web` combines them with `index.html` and `game.pak`; packaging needs no Emscripten.

## Current build

Built on 2026-10-13 with Emscripten 6.0.10, Release, from main (ebabcd7). R7 adds Decal:
box-projected textures/tints with order, volume/normal fades and a cached 1024-square atlas for sixteen
visible projectors. It changes opaque/cutout base color before lighting, preserving depth/picking and
excluding transparent receivers. The light-data texture is 8x33 RGBA32F; SSAO lens settings move into
that texture. All earlier runtime features remain included; see [DECALS.md](../../docs/DECALS.md).
Packaging still patches the game's key into the runtime for keyed OEPAK002 archives and stripped Lua bytecode.

| Artifact | Bytes | SHA256 |
|---|---:|---|
| oe_player.js | 127,633 | 01cab76470319e28cb0ab53673bf1628a9256d96a4bd2704f3153de0186b9a88 |
| oe_player.wasm | 6,065,105 | 0a96ade4aeaec826bcf1abd9ca8bd29ce398cbfc16b5f1407d97eb0de04d2d67 |

Windows Release: 357 tests pass, including decal order/fades, atlas caching, hierarchy/rotation,
texture alpha, cutout holes, displaced shader graphs, lighting and effects, thread-count determinism,
and unchanged depth/IDs/UI. The 800x500 demo's software/D3D11 mean RGB difference is 0.168/255.
Headless Edge (WebGL2 on SwiftShader) compiled and linked the generated mesh program and displayed
the packaged stains/impacts/markings demo without shader/WebGL errors. The mesh fragment program uses
208 vec4 uniform vectors plus 16 sampler uniforms (224 conservatively), and all 16 minimum texture units.
No additional vertex-stage resources; the existing skin/vertex-graph minimum-device path remains unverified.
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
